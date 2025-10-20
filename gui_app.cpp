// gui_app.cpp
#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <unordered_map>
#include <atomic>

// ImGui y backend SDL2 + OpenGL3
#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_sdl2.h"
#include "imgui/backends/imgui_impl_opengl3.h"
#include <SDL.h>
#include <GL/gl.h>

// Nuestros archivos del proyecto
#include "cache.hpp"
#include "pe.h"
#include "shared_memory.h"
#include "shared_memory_adapter.h"
#include "parser.h"

// Función auxiliar para formatear números grandes
template<typename T>
std::string format_number(T num) {
    std::stringstream ss;
    ss << num;
    return ss.str();
}

class GUISystem {
private:
    std::shared_ptr<SharedMemory> shm;
    std::unique_ptr<SharedMemoryAdapter> mem;
    std::unique_ptr<Interconnect> bus;
    std::vector<std::unique_ptr<Cache>> caches;
    std::vector<std::unique_ptr<PE>> pes;
    
    int N = 8;
    std::atomic<bool> system_running{false};
    std::atomic<bool> pause_execution{true};  // Iniciar pausado
    std::atomic<bool> single_step{false};
    int steps_per_frame = 1;

public:
    GUISystem() {
        initialize_system(4, 8);
    }

    ~GUISystem() {
        shutdown_system();
    }

    void shutdown_system() {
        system_running = false;
        pause_execution = true;
        
        // Limpiar en orden seguro
        pes.clear();
        caches.clear();
        
        if (mem) {
            mem.reset();
        }
        
        if (shm) {
            shm->stop();
            shm.reset();
        }
        
        bus.reset();
    }

    void initialize_system(int num_pes, int vector_size) {
        // Primero apagar el sistema anterior de forma segura
        shutdown_system();
        
        // Pequeña pausa para asegurar limpieza
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        
        N = vector_size;
        
        // Crear nuevos objetos
        shm = std::make_shared<SharedMemory>(512);
        shm->start();
        mem = std::make_unique<SharedMemoryAdapter>(shm.get());
        bus = std::make_unique<Interconnect>();
        
        // Crear caches y PEs
        for (int i = 0; i < num_pes; ++i) {
            caches.emplace_back(std::make_unique<Cache>(i, mem.get(), bus.get()));
        }
        
        for (int i = 0; i < num_pes; ++i) {
            pes.emplace_back(std::make_unique<PE>(i, caches[i].get()));
        }
        
        // Inicializar memoria
        initialize_memory();
        
        // Cargar programa
        load_program();
        
        system_running = true;
        pause_execution = true; // Iniciar pausado
        single_step = false;
    }

    void initialize_memory() {
        const size_t baseA_words = 0;
        const size_t baseB_words = baseA_words + static_cast<size_t>(N);
        const size_t baseS_words = baseB_words + static_cast<size_t>(N);
        
        for (int i = 0; i < N; ++i) {
            mem->store64((baseA_words + i) * 8, double(i + 1));
            mem->store64((baseB_words + i) * 8, double((i + 1) * 2));
        }
        
        for (size_t p = 0; p < pes.size(); ++p) {
            mem->store64((baseS_words + p) * 8, 0.0);
        }
    }

    void load_program() {
        std::ifstream fin("dotprod.asm");
        if (!fin) {
            std::cerr << "Error: no se pudo abrir dotprod.asm\n";
            return;
        }
        
        std::stringstream buffer;
        buffer << fin.rdbuf();
        std::vector<Instr> prog;
        std::unordered_map<std::string,size_t> labels;
        parse_asm(buffer.str(), prog, labels);
        
        const size_t baseA_words = 0;
        const size_t baseB_words = baseA_words + static_cast<size_t>(N);
        const size_t baseS_words = baseB_words + static_cast<size_t>(N);
        const int num_pes = pes.size();
        
        const int base_len = N / num_pes;
        const int rest = N % num_pes;
        auto start_index_of = [&](int pe) { return pe * base_len + std::min(pe, rest); };
        auto len_of         = [&](int pe) { return base_len + (pe < rest ? 1 : 0); };
        
        for (int p = 0; p < num_pes; ++p) {
            const int start = start_index_of(p);
            const int len   = len_of(p);
            
            pes[p]->load_program(prog, labels);
            pes[p]->set_reg_int(0, int((baseA_words + start) * 8));
            pes[p]->set_reg_int(1, int((baseB_words + start) * 8));
            pes[p]->set_reg_int(2, int((baseS_words + p) * 8));
            pes[p]->set_reg_int(3, len);
            pes[p]->set_reg_double(4, 0.0);
        }
    }

    void step_system() {
        if (!system_running) return;
        
        // Si estamos en pausa y no es paso simple, no hacer nada
        if (pause_execution && !single_step) return;
        
        if (single_step) {
            // Ejecutar un solo paso en todos los PEs no halt
            for (auto& pe : pes) {
                if (!pe->is_halted()) {
                    pe->step();
                }
            }
            single_step = false;
            pause_execution = true; // Volver a pausar después del paso
            return;
        }
        
        // Ejecución continua
        for (int i = 0; i < steps_per_frame; ++i) {
            bool any_advanced = false;
            for (auto& pe : pes) {
                if (!pe->is_halted()) {
                    pe->step();
                    any_advanced = true;
                }
            }
            
            // Si ningún PE avanzó, pausar automáticamente
            if (!any_advanced) {
                pause_execution = true;
                break;
            }
        }
    }

    void render_gui() {
        // Panel de control principal
        ImGui::Begin("Control del Sistema");
        
        if (ImGui::Button("Reiniciar Sistema")) {
            initialize_system(4, N);
        }
        
        ImGui::SameLine();
        
        // Botón Continuar/Pausar
        if (ImGui::Button(pause_execution ? "Continuar" : "Pausar")) {
            pause_execution = !pause_execution;
            single_step = false; // Cancelar paso simple si estaba activo
        }
        
        ImGui::SameLine();
        
        // Botón Paso Simple
        if (ImGui::Button("Paso Simple")) {
            single_step = true;
            pause_execution = false; // Temporalmente permitir ejecución para el paso
        }
        
        ImGui::SliderInt("Pasos/Frame", &steps_per_frame, 1, 100);
        
        // Control de tamaño N
        static int new_N = N;
        bool n_changed = ImGui::SliderInt("Tamaño N", &new_N, 1, 100);
        ImGui::SameLine();
        if (ImGui::Button("Aplicar N") || n_changed) {
            if (new_N != N) {
                initialize_system(4, new_N);
                N = new_N;
            }
        }
        
        // Estado general del sistema
        ImGui::Separator();
        int running_pes = 0;
        for (auto& pe : pes) {
            if (!pe->is_halted()) running_pes++;
        }
        ImGui::Text("PEs ejecutando: %d/%zu", running_pes, pes.size());
        ImGui::Text("Estado: %s", pause_execution ? "PAUSADO" : "EJECUTANDO");
        if (single_step) {
            ImGui::Text("Modo: Paso Simple");
        }
        ImGui::Text("Pasos por frame: %d", steps_per_frame);
        
        ImGui::End();

        // Panel de PEs
        ImGui::Begin("Processing Elements (PEs)");
        
        if (ImGui::BeginTabBar("PEsTabs")) {
            for (size_t i = 0; i < pes.size(); ++i) {
                if (ImGui::BeginTabItem(("PE" + std::to_string(i)).c_str())) {
                    render_pe_panel(i);
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }
        
        ImGui::End();

        // Panel de Caches
        ImGui::Begin("Caches L1");
        
        if (ImGui::BeginTabBar("CacheTabs")) {
            for (size_t i = 0; i < caches.size(); ++i) {
                if (ImGui::BeginTabItem(("Cache PE" + std::to_string(i)).c_str())) {
                    render_cache_panel(i);
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }
        
        ImGui::End();

        // Panel de Memoria
        ImGui::Begin("Memoria Principal");
        render_memory_panel();
        ImGui::End();

        // Panel de Estadísticas
        ImGui::Begin("Estadísticas");
        render_stats_panel();
        ImGui::End();

        // Panel de Resultados
        ImGui::Begin("Resultados");
        render_results_panel();
        ImGui::End();
    }

private:
    void render_pe_panel(int pe_id) {
        auto& pe = pes[pe_id];
        
        ImGui::Text("PC: %d", pe->get_pc());
        ImGui::Text("Estado: %s", pe->is_halted() ? "HALTED" : "RUNNING");
        
        ImGui::Separator();
        ImGui::Text("Registros:");
        
        for (int i = 0; i < 8; ++i) {
            double reg_val = pe->get_reg_double(i);
            ImGui::Text("R%d: %.2f (int: %d)", i, reg_val, pe->get_reg_int(i));
        }
        
        ImGui::Separator();
        ImGui::Text("Estadísticas PE:");
        ImGui::Text("Loads: %s", format_number(pe->stats.loads).c_str());
        ImGui::Text("Stores: %s", format_number(pe->stats.stores).c_str());
    }

    void render_cache_panel(int cache_id) {
        auto& cache = caches[cache_id];
        auto& stats = cache->stats();
        
        ImGui::Text("Estadísticas Cache:");
        ImGui::Text("Reads: %s", format_number(stats.read_ops).c_str());
        ImGui::Text("Writes: %s", format_number(stats.write_ops).c_str());
        ImGui::Text("Misses: %s", format_number(stats.misses).c_str());
        ImGui::Text("Invalidations: %s", format_number(stats.invalidations).c_str());
        ImGui::Text("Mensajes Bus: %s", format_number(stats.bus_msgs).c_str());
        
        float hit_rate = stats.read_ops > 0 ? 
            (1.0f - (float)stats.misses / stats.read_ops) * 100.0f : 0.0f;
        ImGui::Text("Hit Rate: %.2f%%", hit_rate);
        
        ImGui::Separator();
        ImGui::Text("Estado de Líneas de Cache:");
        
        if (ImGui::BeginChild("CacheLines", ImVec2(0, 300), true)) {
            for (uint32_t set = 0; set < hw::kSets; ++set) {
                ImGui::PushID(set);
                if (ImGui::TreeNode((void*)(intptr_t)set, "Set %d", set)) {
                    for (uint32_t way = 0; way < hw::kWays; ++way) {
                        MESI state = cache->get_state(set, way);
                        uint64_t tag = cache->get_tag(set, way);
                        bool recent = cache->get_recent(set, way);
                        
                        ImGui::Text("Way %d: Tag=0x%lX State=%s LRU=%s",
                                   way, tag, mesi_str(state), recent ? "1" : "0");
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
        }
    }

    void render_memory_panel() {
        const size_t baseA_words = 0;
        const size_t baseB_words = baseA_words + static_cast<size_t>(N);
        const size_t baseS_words = baseB_words + static_cast<size_t>(N);
        
        if (ImGui::BeginTabBar("MemoryTabs")) {
            if (ImGui::BeginTabItem("Vector A")) {
                render_memory_segment(baseA_words, N, "A");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Vector B")) {
                render_memory_segment(baseB_words, N, "B");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Sumas S")) {
                render_memory_segment(baseS_words, pes.size(), "S");
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }

    void render_memory_segment(size_t base, size_t count, const char* prefix) {
        if (ImGui::BeginChild("MemoryView", ImVec2(0, 300), true)) {
            for (size_t i = 0; i < count; ++i) {
                double value = mem->load64((base + i) * 8);
                ImGui::Text("%s[%zu] = %.2f", prefix, i, value);
            }
            ImGui::EndChild();
        }
    }

    void render_stats_panel() {
        ImGui::Text("Estadísticas Globales del Sistema:");
        ImGui::Separator();
        
        uint64_t total_reads = 0, total_writes = 0, total_misses = 0;
        uint64_t total_invalidations = 0, total_bus_msgs = 0;
        
        for (auto& cache : caches) {
            auto& stats = cache->stats();
            total_reads += stats.read_ops;
            total_writes += stats.write_ops;
            total_misses += stats.misses;
            total_invalidations += stats.invalidations;
            total_bus_msgs += stats.bus_msgs;
        }
        
        ImGui::Text("Total Reads: %s", format_number(total_reads).c_str());
        ImGui::Text("Total Writes: %s", format_number(total_writes).c_str());
        ImGui::Text("Total Misses: %s", format_number(total_misses).c_str());
        ImGui::Text("Total Invalidations: %s", format_number(total_invalidations).c_str());
        ImGui::Text("Total Mensajes Bus: %s", format_number(total_bus_msgs).c_str());
        
        float global_hit_rate = total_reads > 0 ? 
            (1.0f - (float)total_misses / total_reads) * 100.0f : 0.0f;
        ImGui::Text("Hit Rate Global: %.2f%%", global_hit_rate);
    }

    void render_results_panel() {
        // Flush caches antes de leer resultados
        for (auto& cache : caches) cache->flush_all();
        if (bus) bus->flush_all();
        
        const size_t baseA_words = 0;
        const size_t baseB_words = baseA_words + static_cast<size_t>(N);
        const size_t baseS_words = baseB_words + static_cast<size_t>(N);
        
        // Calcular resultado
        double total = 0.0;
        for (size_t p = 0; p < pes.size(); ++p) {
            total += mem->load64((baseS_words + p) * 8);
        }
        
        // Calcular esperado
        double expected = 0.0;
        for (int i = 0; i < N; ++i) {
            double a = mem->load64((baseA_words + i) * 8);
            double b = mem->load64((baseB_words + i) * 8);
            expected += a * b;
        }
        
        ImGui::Text("Producto Punto Calculado: %.2f", total);
        ImGui::Text("Producto Punto Esperado:  %.2f", expected);
        
        bool correct = std::abs(total - expected) < 1e-10;
        ImGui::Text("Resultado: %s", correct ? "CORRECTO ✅" : "INCORRECTO ❌");
        
        ImGui::Separator();
        ImGui::Text("Sumas Parciales:");
        for (size_t p = 0; p < pes.size(); ++p) {
            double partial = mem->load64((baseS_words + p) * 8);
            ImGui::Text("S[%zu] = %.2f", p, partial);
        }
    }
};

int main(int, char**) {
    // Inicializar SDL
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::cerr << "Error: " << SDL_GetError() << std::endl;
        return -1;
    }

    // Configurar OpenGL
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

    // Crear ventana
    SDL_Window* window = SDL_CreateWindow(
        "Sistema Multiprocesador MESI - Debugger",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1400, 900,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE
    );

    if (!window) {
        std::cerr << "Error creando ventana SDL: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return -1;
    }

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1); // VSync

    // Inicializar ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    
    // Estilo
    ImGui::StyleColorsDark();
    
    // Inicializar backends
    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 130");

    // Nuestro sistema
    GUISystem gui_system;

    // Loop principal
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT)
                running = false;
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window))
                running = false;
        }

        // Nueva frame ImGui
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        // Ejecutar steps del sistema
        gui_system.step_system();
        
        // Renderizar nuestra GUI
        gui_system.render_gui();

        // Renderizar
        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);

        // Pequeña pausa para no saturar la CPU
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    // Limpiar
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}