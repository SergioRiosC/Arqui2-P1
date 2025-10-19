# ---- toolchain ----
CXX      := g++
CXXFLAGS := -std=c++17 -O2 -pthread -Wall -Wextra -MMD -MP

# ---- fuentes ----
SRC := \
  pe_with_cache.cpp \
  parser.cpp \
  shared_memory.cpp

OBJ := $(SRC:.cpp=.o)
DEP := $(OBJ:.o=.d)

TARGET := sim

# ---- build ----
all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

-include $(DEP)

# ---- util ----
clean:
	rm -f $(OBJ) $(DEP) $(TARGET)

# Pasa N por línea de comando, ej: `make run ARGS=56`
run: $(TARGET)
	./$(TARGET) $(ARGS)
