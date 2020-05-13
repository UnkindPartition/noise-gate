CXXFLAGS += -std=c++14
ng.so: init.o ng.o cmt.o
	$(CXX) -shared -o $@ $+
%.o: %.cpp
	$(CXX) -Wall -pedantic -fPIC -DPIC -O2 $(CXXFLAGS) -o $@ -c $<
install: ng.so
	mkdir -p ~/.ladspa
	cp -f ng.so ~/.ladspa/
