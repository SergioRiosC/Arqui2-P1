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

// CLASE PRINCIPAL DEL SISTEMA - Coordina todos los componentes del multiprocesador
class GUISystem {
private:
    // COMPONENTES DEL SISTEMA MULTIPROCESADOR
    std::shared_ptr<SharedMemory> shm;           // Memoria compartida (512 posiciones)
    std::unique_ptr<SharedMemoryAdapter> mem;    // Adaptador que conecta caches con memoria
    std::unique_ptr<Interconnect> bus;           // Bus de interconexión para protocolo MESI
    std::vector<std::unique_ptr<Cache>> caches;  // 4 caches L1 privadas (una por PE)
    std::vector<std::unique_ptr<PE>> pes;        // 4 Processing Elements
    
    // ESTADO Y CONFIGURACIÓN DEL SISTEMA
    bool final_sum_executed = false;             // Controla si ya se ejecutó la suma final
    std::vector<Instr> program;                  // Programa parseado (instrucciones)
    std::unordered_map<std::string,size_t> labels; // Mapa de etiquetas (MAIN, LOOP, FINAL_SUM)
    int N = 8;                                   // Tamaño de los vectores A y B
    std::atomic<bool> system_running{false};     // Indica si el sistema está activo
    std::atomic<bool> pause_execution{true};     // Control de pausa (inicia pausado)
    std::atomic<bool> single_step{false};        // Bandera para modo paso a paso
    int steps_per_frame = 1;                     // Pasos ejecutados por frame en modo continuo

public:
    // CONSTRUCTOR - Inicializa el sistema con 4 PEs y vectores de tamaño 8
    GUISystem() {
        initialize_system(4, 8);
    }

    // DESTRUCTOR - Garantiza apagado seguro de todos los componentes
    ~GUISystem() {
        shutdown_system();
    }

    // APAGADO SEGURO - Detiene componentes en orden inverso a su creación
    void shutdown_system() {
        system_running = false;
        pause_execution = true;
        
        // Limpiar en orden seguro (inverso al de creación)
        pes.clear();          // 1. Eliminar PEs (detienen ejecución)
        caches.clear();       // 2. Eliminar caches L1
        
        if (mem) {
            mem.reset();      // 3. Eliminar adaptador de memoria
        }
        
        if (shm) {
            shm->stop();      // 4. Detener memoria compartida (para hilo worker)
            shm.reset();
        }
        
        bus.reset();          // 5. Eliminar bus de interconexión
    }

    // INICIALIZACIÓN DEL SISTEMA - Crea y configura todos los componentes
    void initialize_system(int num_pes, int vector_size) {
        // Primero apagar el sistema anterior de forma segura
        shutdown_system();
        
        // Pequeña pausa para asegurar limpieza completa
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        
        N = vector_size;
        
        // CREAR COMPONENTES EN ORDEN JERÁRQUICO:
        // 1. Memoria compartida - almacenamiento principal
        shm = std::make_shared<SharedMemory>(512);
        shm->start();  // Iniciar hilo worker para acceso asíncrono
        
        // 2. Adaptador de memoria - traduce entre caches y memoria compartida
        mem = std::make_unique<SharedMemoryAdapter>(shm.get());
        
        // 3. Bus de interconexión - comunicación para protocolo MESI
        bus = std::make_unique<Interconnect>();
        
        // 4. Caches L1 - una por PE, conectadas al bus y memoria
        for (int i = 0; i < num_pes; ++i) {
            caches.emplace_back(std::make_unique<Cache>(i, mem.get(), bus.get()));
        }
        
        // 5. Processing Elements - unidades de ejecución con caché privada
        for (int i = 0; i < num_pes; ++i) {
            pes.emplace_back(std::make_unique<PE>(i, caches[i].get()));
        }
        
        // CONFIGURACIÓN INICIAL DEL SISTEMA
        initialize_memory();  // Inicializar vectores A, B y sumas parciales S
        load_program();       // Cargar y configurar programa en todos los PEs
        
        // ESTADO INICIAL
        system_running = true;
        pause_execution = true; // Empezar en pausa para permitir inspección
        single_step = false;
        final_sum_executed = false; // Resetear bandera de suma final
    }

    // INICIALIZACIÓN DE MEMORIA - Carga los vectores A, B y prepara sumas parciales
    void initialize_memory() {
        // LAYOUT DE MEMORIA:
        // [0..N-1]: Vector A, [N..2N-1]: Vector B, [2N..2N+3]: Sumas parciales S
        const size_t baseA_words = 0;
        const size_t baseB_words = baseA_words + static_cast<size_t>(N);
        const size_t baseS_words = baseB_words + static_cast<size_t>(N);
        
        // INICIALIZAR VECTOR A: A[i] = i + 1.0
        // INICIALIZAR VECTOR B: B[i] = 2 * (i + 1.0)
        for (int i = 0; i < N; ++i) {
            mem->store64((baseA_words + i) * 8, double(i + 1));
            mem->store64((baseB_words + i) * 8, double((i + 1) * 2));
        }
        
        // INICIALIZAR SUMAS PARCIALES: S[0..3] = 0.0
        for (size_t p = 0; p < pes.size(); ++p) {
            mem->store64((baseS_words + p) * 8, 0.0);
        }
    }

    // CARGA DE PROGRAMA - Lee, parsea y configura el código ASM en todos los PEs
    void load_program() {
        std::ifstream fin("dotprod.asm");
        if (!fin) {
            std::cerr << "Error: no se pudo abrir dotprod.asm\n";
            return;
        }
        
        // LEER ARCHIVO COMPLETO
        std::stringstream buffer;
        buffer << fin.rdbuf();
        
        // PARSEAR: convertir texto ASM a instrucciones ejecutables
        parse_asm(buffer.str(), program, labels);
        
        // CONFIGURACIÓN DE DIRECCIONES DE MEMORIA
        const size_t baseA_words = 0;
        const size_t baseB_words = baseA_words + static_cast<size_t>(N);
        const size_t baseS_words = baseB_words + static_cast<size_t>(N);
        const size_t result_addr = baseS_words + pes.size(); // Posición resultado final
        const int num_pes = pes.size();
        
        // REPARTO BALANCEADO DE TRABAJO - Divide N elementos entre num_pes PEs
        const int base_len = N / num_pes;  // Elementos base por PE
        const int rest = N % num_pes;      // Elementos sobrantes para distribuir
        
        // FUNCIONES LAMBDA para cálculo de segmentos por PE
        auto start_index_of = [&](int pe) { 
            return pe * base_len + std::min(pe, rest); 
        };
        auto len_of = [&](int pe) { 
            return base_len + (pe < rest ? 1 : 0); 
        };
        
        // CONFIGURAR CADA PE CON SU SEGMENTO CORRESPONDIENTE
        for (int p = 0; p < num_pes; ++p) {
            const int start = start_index_of(p); // Índice inicial en vectores A y B
            const int len   = len_of(p);         // Número de elementos a procesar
            
            // Cargar programa idéntico en todos los PEs
            pes[p]->load_program(program, labels);
            
            // CONFIGURAR REGISTROS PARA CÁLCULO PARCIAL:
            pes[p]->set_reg_int(0, int((baseA_words + start) * 8));  // R0 = &A[inicio_segmento]
            pes[p]->set_reg_int(1, int((baseB_words + start) * 8));  // R1 = &B[inicio_segmento]
            pes[p]->set_reg_int(2, int((baseS_words + p) * 8));      // R2 = &S[p] (suma parcial)
            pes[p]->set_reg_int(3, len);                             // R3 = iteraciones (contador)
            pes[p]->set_reg_double(4, 0.0);                          // R4 = acumulador (inicia en 0.0)
        }
        
        // INICIALIZAR POSICIÓN DE RESULTADO FINAL
        mem->store64(result_addr * 8, 0.0);
        final_sum_executed = false; // Resetear bandera de suma final
    }

    // EJECUCIÓN POR PASOS - Avanza la simulación según el modo actual
    void step_system() {
        if (!system_running) return;
        
        // MODO PASO A PASO - Ejecutar una sola instrucción por PE activo
        if (single_step) {
            for (auto& pe : pes) {
                if (!pe->is_halted()) {
                    pe->step(); // Ejecutar una instrucción
                }
            }
            single_step = false;
            pause_execution = true; // Volver a pausa después del paso
            return;
        }
        
        // MODO CONTINUO - Ejecutar múltiples pasos por frame de GUI
        if (!pause_execution) {
            for (int i = 0; i < steps_per_frame; ++i) {
                bool any_advanced = false;
                for (auto& pe : pes) {
                    if (!pe->is_halted()) {
                        pe->step();
                        any_advanced = true;
                    }
                }
                
                // DETECCIÓN AUTOMÁTICA DE FINALIZACIÓN - pausar cuando ningún PE avanza
                if (!any_advanced) {
                    pause_execution = true;
                    break;
                }
            }
        }
    }

    // RENDERIZADO PRINCIPAL DE LA GUI - Coordina todos los paneles de visualización
    void render_gui() {
        // PANEL DE CONTROL PRINCIPAL - controles de ejecución y configuración
        ImGui::Begin("Control del Sistema");
        
        // BOTÓN REINICIAR - recrea todo el sistema desde cero
        if (ImGui::Button("Reiniciar Sistema")) {
            initialize_system(4, N);
        }
        
        ImGui::SameLine();
        
        // BOTÓN CONTINUAR/PAUSAR - alterna ejecución continua
        if (ImGui::Button(pause_execution ? "Continuar" : "Pausar")) {
            pause_execution = !pause_execution;
            single_step = false; // Cancelar paso simple si estaba activo
        }
        
        ImGui::SameLine();
        
        // BOTÓN PASO SIMPLE - ejecuta una instrucción por PE y vuelve a pausa
        if (ImGui::Button("Paso Simple")) {
            single_step = true;
            pause_execution = false; // Temporalmente permitir ejecución para el paso
        }
        
        // CONTROL DE VELOCIDAD - pasos ejecutados por frame en modo continuo
        ImGui::SliderInt("Pasos/Frame", &steps_per_frame, 1, 100);
        
        // CONTROL DE TAMAÑO DE VECTORES - permite cambiar N dinámicamente
        static int new_N = N;
        bool n_changed = ImGui::SliderInt("Tamaño N", &new_N, 1, 100);
        ImGui::SameLine();
        if (ImGui::Button("Aplicar N") || n_changed) {
            if (new_N != N) {
                initialize_system(4, new_N); // Reiniciar sistema con nuevo N
                N = new_N;
            }
        }
        
        // INFORMACIÓN DE ESTADO GENERAL
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

        // PANEL DE PROCESSING ELEMENTS - estado individual de cada PE
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

        // PANEL DE CACHES L1 - estado y estadísticas de cada caché
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

        // PANEL DE MEMORIA PRINCIPAL - visualización de vectores A, B y sumas S
        ImGui::Begin("Memoria Principal");
        render_memory_panel();
        ImGui::End();

        // PANEL DE ESTADÍSTICAS - métricas globales del sistema
        ImGui::Begin("Estadísticas");
        render_stats_panel();
        ImGui::End();

        // PANEL DE RESULTADOS - producto punto calculado y validación
        ImGui::Begin("Resultados");
        render_results_panel();
        ImGui::End();
    }

private:
    // RENDERIZADO DE PANEL DE PE INDIVIDUAL - muestra estado y registros
    void render_pe_panel(int pe_id) {
        auto& pe = pes[pe_id];
        
        // INFORMACIÓN BÁSICA DEL PE
        ImGui::Text("PC: %d", pe->get_pc());
        ImGui::Text("Estado: %s", pe->is_halted() ? "HALTED" : "RUNNING");
        
        ImGui::Separator();
        ImGui::Text("Registros:");
        
        // MOSTRAR LOS 8 REGISTROS (R0-R7) como doubles y enteros
        for (int i = 0; i < 8; ++i) {
            double reg_val = pe->get_reg_double(i);
            ImGui::Text("R%d: %.2f (int: %d)", i, reg_val, pe->get_reg_int(i));
        }
        
        // ESTADÍSTICAS SIMPLES DEL PE
        ImGui::Separator();
        ImGui::Text("Estadísticas PE:");
        ImGui::Text("Loads: %s", format_number(pe->stats.loads).c_str());
        ImGui::Text("Stores: %s", format_number(pe->stats.stores).c_str());
    }

    // RENDERIZADO DE PANEL DE CACHÉ - muestra estadísticas y estado de líneas
    void render_cache_panel(int cache_id) {
        auto& cache = caches[cache_id];
        auto& stats = cache->stats();
        
        // ESTADÍSTICAS DE LA CACHÉ
        ImGui::Text("Estadísticas Cache:");
        ImGui::Text("Reads: %s", format_number(stats.read_ops).c_str());
        ImGui::Text("Writes: %s", format_number(stats.write_ops).c_str());
        ImGui::Text("Misses: %s", format_number(stats.misses).c_str());
        ImGui::Text("Invalidations: %s", format_number(stats.invalidations).c_str());
        ImGui::Text("Mensajes Bus: %s", format_number(stats.bus_msgs).c_str());
        ImGui::Text("Write-backs: %s", format_number(stats.writebacks).c_str());
        ImGui::Text("Upgrades a M: %s", format_number(stats.upgrades).c_str()); 
        
        // CÁLCULO DE TASA DE ACIERTOS (HIT RATE)
        float hit_rate = stats.read_ops > 0 ? 
            (1.0f - (float)stats.misses / stats.read_ops) * 100.0f : 0.0f;
        ImGui::Text("Hit Rate: %.2f%%", hit_rate);
        
        // ESTADO DETALLADO DE LÍNEAS DE CACHÉ
        ImGui::Separator();
        ImGui::Text("Estado de Líneas de Cache:");
        
        if (ImGui::BeginChild("CacheLines", ImVec2(0, 300), true)) {
            // RECORRER TODOS LOS SETS (8 sets en total)
            for (uint32_t set = 0; set < hw::kSets; ++set) {
                ImGui::PushID(set);
                if (ImGui::TreeNode((void*)(intptr_t)set, "Set %d", set)) {
                    // RECORRER AMBOS WAYS POR SET (2-way associative)
                    for (uint32_t way = 0; way < hw::kWays; ++way) {
                        MESI state = cache->get_state(set, way);
                        uint64_t tag = cache->get_tag(set, way);
                        bool recent = cache->get_recent(set, way);
                        
                        // MOSTRAR INFORMACIÓN DE LA LÍNEA
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

    // RENDERIZADO DE PANEL DE MEMORIA - muestra vectores A, B y sumas parciales
    void render_memory_panel() {
        const size_t baseA_words = 0;
        const size_t baseB_words = baseA_words + static_cast<size_t>(N);
        const size_t baseS_words = baseB_words + static_cast<size_t>(N);
        
        // PESTAÑAS PARA DIFERENTES SECCIONES DE MEMORIA
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

    // RENDERIZADO DE SEGMENTO DE MEMORIA - muestra un rango de direcciones
    void render_memory_segment(size_t base, size_t count, const char* prefix) {
        if (ImGui::BeginChild("MemoryView", ImVec2(0, 300), true)) {
            for (size_t i = 0; i < count; ++i) {
                double value = mem->load64((base + i) * 8);
                ImGui::Text("%s[%zu] = %.2f", prefix, i, value);
            }
            ImGui::EndChild();
        }
    }

    // RENDERIZADO DE ESTADÍSTICAS GLOBALES - suma estadísticas de todas las caches
    void render_stats_panel() {
        ImGui::Text("Estadísticas Globales del Sistema:");
        ImGui::Separator();
        
        // ACUMULAR ESTADÍSTICAS DE TODAS LAS CACHES
        uint64_t total_reads = 0, total_writes = 0, total_misses = 0;
        uint64_t total_invalidations = 0, total_bus_msgs = 0;
        uint64_t total_writebacks = 0, total_upgrades = 0; 
        
        for (auto& cache : caches) {
            auto& stats = cache->stats();
            total_reads += stats.read_ops;
            total_writes += stats.write_ops;
            total_misses += stats.misses;
            total_invalidations += stats.invalidations;
            total_bus_msgs += stats.bus_msgs;
            total_writebacks += stats.writebacks;
            total_upgrades += stats.upgrades; 
        }
        
        // MOSTRAR ESTADÍSTICAS ACUMULADAS
        ImGui::Text("Total Reads: %s", format_number(total_reads).c_str());
        ImGui::Text("Total Writes: %s", format_number(total_writes).c_str());
        ImGui::Text("Total Misses: %s", format_number(total_misses).c_str());
        ImGui::Text("Total Invalidations: %s", format_number(total_invalidations).c_str());
        ImGui::Text("Total Mensajes Bus: %s", format_number(total_bus_msgs).c_str());
        ImGui::Text("Total Write-backs: %s", format_number(total_writebacks).c_str());
        ImGui::Text("Total Upgrades a M: %s", format_number(total_upgrades).c_str());
        
        // CALCULAR Y MOSTRAR HIT RATE GLOBAL
        float global_hit_rate = total_reads > 0 ? 
            (1.0f - (float)total_misses / total_reads) * 100.0f : 0.0f;
        ImGui::Text("Hit Rate Global: %.2f%%", global_hit_rate);
    }

    // RENDERIZADO DE PANEL DE RESULTADOS - muestra producto punto y validación
    void render_results_panel() {
        // FORZAR FLUSH DE CACHES - garantiza que memoria tenga datos actualizados
        for (auto& cache : caches) cache->flush_all();
        if (bus) bus->flush_all();
        
        // CONFIGURACIÓN DE DIRECCIONES DE MEMORIA
        const size_t baseA_words = 0;
        const size_t baseB_words = baseA_words + static_cast<size_t>(N);
        const size_t baseS_words = baseB_words + static_cast<size_t>(N);
        const size_t result_addr = baseS_words + pes.size();
        
        // VERIFICAR SI TODOS LOS PEs TERMINARON CÁLCULO PARCIAL
        bool all_partial_done = true;
        for (auto& pe : pes) {
            if (!pe->is_halted()) {
                all_partial_done = false;
                break;
            }
        }
        
        // DETECCIÓN Y CONFIGURACIÓN DE SUMA FINAL
        // Solo ejecutar una vez cuando todos los PEs terminen cálculo parcial
        if (all_partial_done && !final_sum_executed) {
            // Buscar posición de la etiqueta FINAL_SUM en el programa
            auto it = labels.find("FINAL_SUM");
            if (it != labels.end()) {
                // RECONFIGURAR PE0 PARA EJECUTAR SUMA FINAL
                pes[0]->set_pc(it->second); // Saltar directamente a FINAL_SUM
                pes[0]->set_reg_int(0, int(baseS_words * 8));     // R0 = &S[0] (base sumas)
                pes[0]->set_reg_int(1, int(pes.size()));          // R1 = 4 (número de PEs)
                pes[0]->set_reg_int(2, int(result_addr * 8));     // R2 = dirección resultado
                pes[0]->set_reg_double(4, 0.0);                   // R4 = acumulador (reset)
            }
            final_sum_executed = true; // Marcar que suma final está en progreso
        }
        
        // LEER RESULTADO ACTUAL DE LA MEMORIA
        double total = mem->load64(result_addr * 8);
        
        // CALCULAR VALOR ESPERADO (secuencial) para validación
        double expected = 0.0;
        for (int i = 0; i < N; ++i) {
            double a = mem->load64((baseA_words + i) * 8);
            double b = mem->load64((baseB_words + i) * 8);
            expected += a * b;
        }
        
        // MOSTRAR RESULTADOS Y VALIDACIÓN
        ImGui::Text("Producto Punto Calculado: %.2f", total);
        ImGui::Text("Producto Punto Esperado:  %.2f", expected);
        
        // VALIDAR PRECISIÓN (tolerancia 1e-10 para doubles)
        bool correct = std::abs(total - expected) < 1e-10;
        ImGui::Text("Resultado: %s", correct ? "CORRECTO" : "INCORRECTO");
        
        // MOSTRAR SUMAS PARCIALES DE CADA PE
        ImGui::Separator();
        ImGui::Text("Sumas Parciales:");
        for (size_t p = 0; p < pes.size(); ++p) {
            double partial = mem->load64((baseS_words + p) * 8);
            ImGui::Text("S[%zu] = %.2f", p, partial);
        }
    }
};

// FUNCIÓN PRINCIPAL - Punto de entrada de la aplicación GUI
int main(int, char**) {
    // INICIALIZACIÓN DE SDL (Simple DirectMedia Layer)
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::cerr << "Error: " << SDL_GetError() << std::endl;
        return -1;
    }

    // CONFIGURACIÓN DE OPENGL
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

    // CREACIÓN DE VENTANA PRINCIPAL
    SDL_Window* window = SDL_CreateWindow(
        "Sistema Multiprocesador MESI - Debugger",  // Título
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, // Posición centrada
        1400, 900,                                   // Tamaño (ancho x alto)
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE     // Flags: OpenGL + redimensionable
    );

    if (!window) {
        std::cerr << "Error creando ventana SDL: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return -1;
    }

    // CONTEXTO OPENGL
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1); // VSync activado

    // INICIALIZACIÓN DE ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    
    // CONFIGURACIÓN DE ESTILO (tema oscuro)
    ImGui::StyleColorsDark();
    
    // INICIALIZACIÓN DE BACKENDS (SDL2 + OpenGL3)
    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 130");

    // CREACIÓN DEL SISTEMA MULTIPROCESADOR
    GUISystem gui_system;

    // LOOP PRINCIPAL DE LA APLICACIÓN
    bool running = true;
    while (running) {
        // PROCESAMIENTO DE EVENTOS SDL
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event); // Pasar eventos a ImGui
            if (event.type == SDL_QUIT)
                running = false;
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window))
                running = false;
        }

        // PREPARAR NUEVO FRAME DE ImGui
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        // EJECUTAR SIMULACIÓN - avanzar según modo actual (pausa/continuo/paso)
        gui_system.step_system();
        
        // RENDERIZAR INTERFAZ GRÁFICA
        gui_system.render_gui();

        // RENDERIZADO FINAL
        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f); // Color de fondo oscuro
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window); // Intercambiar buffers

        // PEQUEÑA PAUSA PARA NO SATURAR LA CPU (~60 FPS)
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    // LIMPIEZA FINAL - destruir recursos en orden inverso
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}