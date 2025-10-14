CXX = g++
CXXFLAGS = -std=c++17 -O2 -pthread

all: pe

pe: pe.o parser.o
	$(CXX) $(CXXFLAGS) -o pe pe.o parser.o

pe.o: pe.cpp parser.h instr.h
	$(CXX) $(CXXFLAGS) -c pe.cpp

parser.o: parser.cpp parser.h instr.h
	$(CXX) $(CXXFLAGS) -c parser.cpp

clean:
	rm -f *.o pe
