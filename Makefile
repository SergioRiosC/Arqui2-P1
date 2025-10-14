CXX = g++
CXXFLAGS = -std=c++17 -O2 -pthread
SOURCES = pe.cpp parser.cpp cache.cpp
OBJECTS = $(SOURCES:.cpp=.o)
TARGET = pe

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(OBJECTS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) $(TARGET)
