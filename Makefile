CC=c++
CXXFLAGS=-O2 -g -fPIC
LD=c++
LDFLAGS=-shared
LDLIBS=-lpthread -ldl
MODULES=oomkiller.so 

all:	threadhelper $(MODULES)

%.o: %.cc module.h Makefile
	$(CXX) $(CXXFLAGS) -c -o $@ $<

%.so: %.o globfile.h
	$(CXX) $(CXXFLAGS) -shared -o $@ $<

threadhelper: threadhelper.o
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDLIBS)

threadhelper.o: threadhelper.cxx module.h Makefile
	$(CXX) -c $(CXXFLAGS) -o $@ $<

clean:
	rm -fr threadhelper.o threadhelper $(MODULES)
