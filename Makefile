CXXFLAGS += -std=c++14
OBJ_FILES = init.o ng.o cmt.o
ng.so: ${OBJ_FILES}
	$(CXX) -shared -o $@ $+
%.o: %.cpp
	$(CXX) -Wall -pedantic -fPIC -DPIC -O2 $(CXXFLAGS) -o $@ -c $<
clean:
	rm -f ${OBJ_FILES} ng.so
install: ng.so
	mkdir -p ~/.ladspa
	cp -f ng.so ~/.ladspa/
