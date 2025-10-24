# Compilador y flags
CXX = g++
CXXFLAGS = -std=c++17 -O2 -pthread -Wall -Wextra
TARGET_STEPPER = stepper_app

# Archivos fuente comunes
COMMON_SOURCES = cache.cpp pe.cpp shared_memory.cpp parser.cpp

# Archivos fuente especificos
SIM_SOURCES = pe_with_cache.cpp
STEPPER_SOURCES = sim_step.cpp

TARGET_GUI = gui_app
GUI_SOURCES = gui_app.cpp

# Dependencias para GUI
GUI_DEPS = imgui/imgui.cpp imgui/imgui_draw.cpp imgui/imgui_tables.cpp \
           imgui/imgui_widgets.cpp imgui/backends/imgui_impl_sdl2.cpp \
           imgui/backends/imgui_impl_opengl3.cpp

# Flags para GUI
GUI_CXXFLAGS = $(CXXFLAGS) -Iimgui -Iimgui/backends `sdl2-config --cflags`
GUI_LDFLAGS = `sdl2-config --libs` -lGL

# Reglas principales
all: $(TARGET_GUI) $(TARGET_STEPPER)

$(TARGET_STEPPER): $(STEPPER_SOURCES) $(COMMON_SOURCES)
	$(CXX) $(CXXFLAGS) -o $(TARGET_STEPPER) $(STEPPER_SOURCES) $(COMMON_SOURCES)


$(TARGET_GUI): $(GUI_SOURCES) $(COMMON_SOURCES) $(GUI_DEPS)
	$(CXX) $(GUI_CXXFLAGS) -o $(TARGET_GUI) $(GUI_SOURCES) $(COMMON_SOURCES) $(GUI_DEPS) $(GUI_LDFLAGS)

# Reglas cortas
stepper: $(TARGET_STEPPER)
gui: $(TARGET_GUI)  # Esta linea esta bien

# Reglas de ejecucion
run: $(TARGET_STEPPER)
	./$(TARGET_STEPPER) 4 8

run-gui: $(TARGET_GUI)
	./$(TARGET_GUI)

# Reglas de limpieza
clean:
	rm -f $(TARGET_SIM) $(TARGET_STEPPER) $(TARGET_GUI) *.o

clean-all: clean
	rm -f *.gch

# Dependencias
pe_with_cache.cpp: pe.h cache.hpp shared_memory.h shared_memory_adapter.h parser.h instr.h
sim_step.cpp: pe.h cache.hpp shared_memory.h shared_memory_adapter.h parser.h instr.h
gui_app.cpp: pe.h cache.hpp shared_memory.h shared_memory_adapter.h parser.h instr.h
pe.cpp: pe.h cache.hpp instr.h
cache.cpp: cache.hpp shared_memory.h shared_memory_adapter.h
shared_memory.cpp: shared_memory.h
parser.cpp: parser.h instr.h

.PHONY: all sim stepper gui run run-stepper run-big run-stepper-big run-gui clean clean-all help