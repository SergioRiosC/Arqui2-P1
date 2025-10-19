# Makefile para el simulador PE + Cache
# Detecta automáticamente si existe cache.cpp y lo incluye en la compilación.

CXX := g++
CXXFLAGS := -std=c++17 -O2 -pthread -Wall -Wextra

# Archivos fuente "obligatorios"
SRCS := pe_with_cache.cpp parser.cpp

# Si existe cache.cpp lo añadimos; si no, asumimos header-only (cache.hpp)
ifneq ($(wildcard cache.cpp),)
SRCS += cache.cpp
endif

OBJS := $(SRCS:.cpp=.o)
TARGET := sim

.PHONY: all clean run debug rebuild

all: $(TARGET)

# linking
$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS)

# compilación de cada .cpp -> .o
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# objetivo para depuración (sin optimización, con símbolos)
debug: CXXFLAGS := -std=c++17 -g -O0 -pthread -Wall -Wextra
debug: clean $(TARGET)

# fuerza recompilar todo
rebuild: clean all

# ejecuta el binario (útil desde make)
run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)

# muestra variables (útil para debugging del Makefile)
show:
	@echo "SRCS = $(SRCS)"
	@echo "OBJS = $(OBJS)"
	@echo "CXXFLAGS = $(CXXFLAGS)"
