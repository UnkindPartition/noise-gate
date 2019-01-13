This is a better noise gate for Audacity, Ardour, or any other editor/DAW that supports the
[LADSPA](https://www.ladspa.org/) plugin interface.

See <https://ro-che.info/articles/2019-01-12-better-noise-gate> for the
introduction.

## Installation

1. Make sure you have a standard C++ development environment (the compiler and
   the Boost library).
1. Make sure that you have the LADSPA SDK installed, which consists of a single
   header file, `ladspa.h`. For example, on Fedora you have to install the
   `ladspa-devel` package.
1. Run `make`. This should produce a file named `ng.so` on UNIX, `ng.dylib` on
   MacOS, or `ng.dll` on Windows. This is the plugin itself.
1. Change the `LADSPA_PATH` to include the directory containing the plugin (i.e.
   `ng.so` or similar). Personally, I set my `LADSPA_PATH` to
   `/home/feuerbach/.ladspa:/usr/lib64/ladspa` in `~/.pam_environment`, and then
   copy the plugin to `~/.ladspa`, but your actions may differ depending on the
   platform.

The plugin will show up under the name "Roman's noise gate".

These instructions have been tested on Linux. Building on other systems
may require minor tweaks in the instructions and the `Makefile`. If you've
installed the plugin on a different system, please send a pull request with the
instructions for that system.
