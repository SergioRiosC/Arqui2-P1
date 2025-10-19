# Compilador y flags
CXX = g++
CXXFLAGS = -std=c++17 -O2 -pthread -Wall -Wextra
TARGET_SIM = sim
TARGET_STEPPER = stepper

# Archivos fuente comunes
COMMON_SOURCES = cache.cpp pe.cpp shared_memory.cpp parser.cpp

# Archivos fuente específicos
SIM_SOURCES = pe_with_cache.cpp
STEPPER_SOURCES = sim_step.cpp

# Reglas principales
all: $(TARGET_SIM)

$(TARGET_SIM): $(SIM_SOURCES) $(COMMON_SOURCES)
	$(CXX) $(CXXFLAGS) -o $(TARGET_SIM) $(SIM_SOURCES) $(COMMON_SOURCES)

$(TARGET_STEPPER): $(STEPPER_SOURCES) $(COMMON_SOURCES)
	$(CXX) $(CXXFLAGS) -o $(TARGET_STEPPER) $(STEPPER_SOURCES) $(COMMON_SOURCES)

# Reglas cortas
sim: $(TARGET_SIM)
stepper: $(TARGET_STEPPER)

# Reglas de ejecución
run: $(TARGET_SIM)
	./$(TARGET_SIM) 8

run-stepper: $(TARGET_STEPPER)
	./$(TARGET_STEPPER) 4 8

run-big: $(TARGET_SIM)
	./$(TARGET_SIM) 64

run-stepper-big: $(TARGET_STEPPER)
	./$(TARGET_STEPPER) 4 64

# Reglas de limpieza
clean:
	rm -f $(TARGET_SIM) $(TARGET_STEPPER) *.o

clean-all: clean
	rm -f *.gch

# Reglas de ayuda
help:
	@echo "Makefile para Sistema Multiprocesador con Coherencia MESI"
	@echo ""
	@echo "Targets disponibles:"
	@echo "  all, sim      - Compila la simulación normal (default)"
	@echo "  stepper       - Compila el stepper/debugger"
	@echo "  run           - Ejecuta simulación con N=8"
	@echo "  run-stepper   - Ejecuta stepper con 4 PEs y N=8"
	@echo "  run-big       - Ejecuta simulación con N=64"
	@echo "  run-stepper-big - Ejecuta stepper con 4 PEs y N=64"
	@echo "  clean         - Elimina ejecutables"
	@echo "  clean-all     - Elimina ejecutables y headers precompilados"
	@echo "  help          - Muestra esta ayuda"
	@echo ""
	@echo "Uso manual:"
	@echo "  ./sim [N]              - Simulación con N elementos (default 8)"
	@echo "  ./stepper [PEs] [N]    - Stepper con PEs procesadores y N elementos"
	@echo "                           (default: 4 PEs, 8 elementos)"

# Dependencias
pe_with_cache.cpp: pe.h cache.hpp shared_memory.h shared_memory_adapter.h parser.h instr.h
sim_step.cpp: pe.h cache.hpp shared_memory.h shared_memory_adapter.h parser.h instr.h
pe.cpp: pe.h cache.hpp instr.h
cache.cpp: cache.hpp shared_memory.h shared_memory_adapter.h
shared_memory.cpp: shared_memory.h
parser.cpp: parser.h instr.h

.PHONY: all sim stepper run run-stepper run-big run-stepper-big clean clean-all help