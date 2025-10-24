# Sistema Multiprocesador con Coherencia de Caché MESI

Este proyecto implementa un simulador de sistema multiprocesador con protocolo de coherencia MESI para el cálculo paralelo del producto punto de vectores.

Estudiantes:
    - Sergio Rios Campos
    - Fabian Crawford Barquero
    - Sebastian Quesada Rojas

## Características Principales

- **4 Processing Elements (PEs)** con cachés L1 privadas
- **Protocolo MESI** para coherencia de caché
- **Memoria compartida** con acceso asíncrono
- **Interfaz gráfica** para visualización en tiempo real
- **CLI stepper** para depuración paso a paso
- **Cachés 2-way set associative** con políticas write-back/write-allocate


### Dependencias

**Ubuntu/Debian:**

ImGui:
Una vez descargado nuestro proyecto, es necesario instalar **ImGUI** con el siguiente comando, en la raiz del proyecto:
```bash
git clone https://github.com/ocornut/imgui.git
```
Posteriormente, es necesario instalar las siguientes dependencias:
```bash
sudo apt-get update
sudo apt-get install libsdl2-dev libgl1-mesa-dev
```
## Compilacion y Ejecucion
### Compilar solo la GUI
```bash
make gui
```
### Compilar solo el stepper CLI
```bash
make stepper
```
### Compilar ambos
```bash
make all
```
### Limpiar archivos compilados
```bash
make clean
```
Para la ejecucuin del proyecto se tienen las siguientes opciones:

### Ejecutar GUI
```bash
./gui
```
Al ejecutar la GUI, vera varias ventanas con informacion variada y una de control, en ella podra definir el numero de posiciones de los vectores, tamaño de step, reinicio del sistema, aplicar tamaño de N, ejecucion hasta el final o ir paso a paso.
### Ejecutar Stepper (programa por CLI)
```bash
./stepper N
```
Donde N es el numero de posiciones de los vectores A y B (hasta 253)

Al ejecutar ya sea el CLI, verá un menu de ayuda con las distintas opciones a poder ejecutar, solo escriba la que desea y esta se ejecutará. 