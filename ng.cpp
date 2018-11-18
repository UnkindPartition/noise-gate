#include <ladspa.h>
#include "cmt.h"
#include <cmath>
#include <deque>
#include <boost/circular_buffer.hpp>

using namespace std;

// A sliding window that maintains its maximum absolute value
class MaxWindow {
  private:
    // Window size.
    deque<LADSPA_Data>::size_type window_size;
    // Samples within the window.
    boost::circular_buffer<LADSPA_Data> buf;
    // Indinces into the whole track (not buf!), corresponding to the decreasing
    // subsequence of samples within the current window.
    deque<unsigned long> indices;
    // Total cumulative number of samples pushed into this window.
    // Used to convert 'indices' to actual buf indices.
    unsigned long n_samples {0};
    // Get sample value by its absolute index.
    inline LADSPA_Data get_sample(unsigned long index) const {
      return buf[buf.size() - (n_samples - index)];
    }
  public:
    MaxWindow(deque<LADSPA_Data>::size_type window_size)
      : window_size(window_size), buf(window_size) {};
    void push(LADSPA_Data sample) {
      sample = abs(sample);
      while (!indices.empty() && get_sample(indices.back()) <= sample) {
        indices.pop_back();
      }
      while (!indices.empty() && indices.front() <= n_samples - window_size) {
        indices.pop_front();
      }
      indices.push_back(n_samples++);
      buf.push_back(sample);
    }
    LADSPA_Data level() const {
      return get_sample(indices.front());
    }
};

// A sliding window that knows at each moment how much non-silence it contains.
//
// The window has the latency ns_window_size.
class NonSilenceWindow {
  private:
    boost::circular_buffer<bool> buf; // true == non-silent
    MaxWindow max_window;
    LADSPA_Data sample_rate;
    LADSPA_Data level_threshold; // a threshold above which the sound is considered non-silent
    unsigned long nonsilent_samples {0};
  public:
    NonSilenceWindow(boost::circular_buffer<LADSPA_Data>::capacity_type ns_window_size,
                     boost::circular_buffer<LADSPA_Data>::capacity_type max_window_size,
                     LADSPA_Data sample_rate,
                     LADSPA_Data level_threshold)
      : buf(ns_window_size), max_window(max_window_size),
        sample_rate(sample_rate), level_threshold(level_threshold)
      {};
    void push(LADSPA_Data sample) {
      max_window.push(sample);
      if (buf.full())
        nonsilent_samples -= buf.front();
      bool new_nonsilent = max_window.level() >= level_threshold;
      buf.push_back(new_nonsilent);
      nonsilent_samples += new_nonsilent;
    }
    // Get the total amount of non-silence inside the window in seconds
    LADSPA_Data nonsilent() const {
      return nonsilent_samples / sample_rate;
    }
};

// A window that smoothes the transition between the open and closed states of
// the gate.
//
// The state of the gate is represented by a bool: true = open, false = closed.
//
// When the gate moves from open to closed (true -> false), the gate closes
// smoothly after that.
//
// When the gate moves from closed to open (false -> true), this event is
// anticipated ahead of time and the transition is again smoothed.
//
// This function could probably be optimized by introducing more states and
// avoiding multiplications when the gate remains open or closed for a long time.
class SmoothingWindow {
  private:
    const LADSPA_Data floor = 1e-4; // -80 dB
    unsigned long window_size;
    // The current scaling factor applied to the sound samples.
    LADSPA_Data current_coef = 1;
    // Are we currently rising (true) or falling (false)?
    bool rising {true};
    // The number of samples since we've last seen the gate open.
    // If it's more than the window size, we may begin to decrease the scaling
    // factor.
    long unsigned samples_since_open = 0;
    // factor is initialized in the constructor based on
    // the window size and then never changes.
    LADSPA_Data factor;
  public:
    SmoothingWindow(unsigned long window_size)
      : window_size(window_size),
        factor(exp(-log(floor)/window_size))
      {}
    // Push a new sample (is the gate open?)
    void push(bool open) {
      if (open) {
        samples_since_open = 0;
        rising = true;
      } else {
        samples_since_open++;
        if (samples_since_open > window_size) {
          rising = false;
        }
      }
      if (rising) {
        current_coef = min(max(current_coef, floor) * factor, 1.f);
      } else {
        current_coef = current_coef / factor;
        if (current_coef < floor) {
          current_coef = 0.f;
        }
      }
    }
    // Get the current scaling factor (with the latency equal to the
    // attack/decay duration)
    LADSPA_Data scaling_factor() const {
      return current_coef;
    }
};

const unsigned long port_count = 7;

class NoiseGate : public CMT_PluginInstance {
public:
  unsigned sample_rate;
  unique_ptr<NonSilenceWindow> ns_window;
  unique_ptr<SmoothingWindow>  sm_window;
  unique_ptr<boost::circular_buffer<LADSPA_Data>> buf;

  // NB: we cannot do much initialization in the constructor because the ports
  // may be connected after it is called.
  NoiseGate(const LADSPA_Descriptor *,
            unsigned sample_rate)
    : CMT_PluginInstance(port_count), sample_rate(sample_rate) {}

  void run(unsigned long n_samples) {

    LADSPA_Data threshold     = pow(10.f, *(m_ppfPorts[0]) / 20.f);
    LADSPA_Data window_size   = *(m_ppfPorts[1]) / 1000; // in seconds
    LADSPA_Data min_nonsilent = *(m_ppfPorts[2]) / 1000; // in seconds
    LADSPA_Data attack        = *(m_ppfPorts[3]) / 1000; // in seconds
    LADSPA_Data *input        = m_ppfPorts[4];
    LADSPA_Data *output       = m_ppfPorts[5];
    LADSPA_Data *latency      = m_ppfPorts[6];

    unsigned half_window_samples = window_size * sample_rate / 2.f;
    unsigned window_samples = 2 * half_window_samples + 1;
    unsigned sm_window_size = attack * sample_rate;
    unsigned latency_samples = half_window_samples + sm_window_size;
    *latency = latency_samples;

    if (ns_window == nullptr) {
      ns_window = make_unique<NonSilenceWindow>(window_samples,
                                                 sample_rate * 5e-3,
                                                 sample_rate,
                                                 threshold);
    }
    if (sm_window == nullptr) {
      sm_window = make_unique<SmoothingWindow>(sm_window_size);
    }
    if (buf == nullptr) {
      buf = make_unique<boost::circular_buffer<LADSPA_Data>>(latency_samples);
    }
    for (unsigned i = 0; i < n_samples; i++) {
      // save the sample so we don't lose it after writing to output[i]
      LADSPA_Data sample = input[i];
      ns_window->push(sample);
      sm_window->push(ns_window->nonsilent() >= min_nonsilent);
      if (buf->full()) {
        output[i] = buf->front() * sm_window->scaling_factor();
      }
      else {
        output[i] = 0;
      }
      buf->push_back(sample);
    }
  }

  void run_noise_gate(LADSPA_Handle Instance, unsigned long SampleCount);
};

void run_noise_gate (LADSPA_Handle handle,
                     unsigned long n_samples) {
  NoiseGate *ng = static_cast<NoiseGate *>(handle);

  ng->run(n_samples);
}
void init_noise_gate() {
  CMT_Descriptor *desc = new CMT_Descriptor
    (5581,
     "noise_gate",
     0, // Properties
     "Roman's Noise Gate",
     "Roman Cheplyaka",
     "(c) Roman Cheplyaka 2018",
     nullptr, // ImplementationData
     CMT_Instantiate<NoiseGate>,
     nullptr, // activate
     run_noise_gate,
     nullptr, // run_adding
     nullptr, // set_run_adding_gain
     nullptr  // deactivate TODO we need to clear the buffers/windows
     );
  desc->addPort
    (LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
     "Threshold (dB)",
     LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE,
     -80, 0);
  desc->addPort
    (LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
     "Window size (ms)",
     LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE,
     100, 3000);
  desc->addPort
    (LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
     "Non-silent audio per window (ms)",
     LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE,
     50, 500);
  desc->addPort
    (LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
     "Attack/decay (ms)",
     LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE,
     10, 200);
  desc->addPort
    (LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO,
     "Input");
  desc->addPort
    (LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO,
     "Output");
  desc->addPort
    (LADSPA_PORT_OUTPUT | LADSPA_PORT_CONTROL,
     "latency");
  registerNewPluginDescriptor(desc);
}
