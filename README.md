# Látigo Interactivo en Tiempo Real

Simulación de un látigo con física de Verlet, renderizado con OpenGL y una ventana/capa transparente. El látigo sigue al cursor del ratón y reproduce un sonido de "chasquido" cuando la punta supera una velocidad umbral.

- **Linux / Hyprland**: se ejecuta como una **capa Wayland real** (`layer-shell` en modo `OVERLAY`) sobre todo el escritorio.
- **Windows 11**: se ejecuta como una **ventana GLFW transparente** con estilos nativos `WS_EX_LAYERED | WS_EX_TOPMOST`.

---

## Requisitos

### Linux (Hyprland / Wayland)

- Distro con soporte Wayland y Hyprland (u otro compositor compatible con `wlr-layer-shell`).
- `cmake`, `g++` o `clang++`.
- Paquetes de desarrollo:
  - `wayland-client`
  - `wayland-egl`
  - `egl`
  - `gl`
  - `wayland-scanner` (suele venir con `wayland`)

Ejemplo en Arch:

```bash
sudo pacman -S base-devel cmake wayland wayland-protocols mesa
```

Ejemplo en Debian/Ubuntu:

```bash
sudo apt install build-essential cmake libwayland-dev wayland-scanner libegl1-mesa-dev libgl1-mesa-dev
```

### Windows 11

- Visual Studio 2022 con la carga de trabajo **"Desarrollo para el escritorio con C++"**, o MinGW-w64.
- [CMake](https://cmake.org/download/).
- [GLFW3](https://www.glfw.org/). La forma más sencilla es instalarlo con **vcpkg**:

```cmd
vcpkg install glfw3
```

---

## Compilación

### Linux / Hyprland

```bash
cd /ruta/al/proyecto
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### Windows 11 (con Visual Studio + vcpkg)

Abre **Developer Command Prompt for VS** o PowerShell:

```cmd
cd C:\ruta\al\proyecto
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:\ruta\a\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
```

> Sustituye `C:\ruta\a\vcpkg` por la ubicación real de tu instalación de vcpkg.

### Cross-compilar para Windows desde Linux (MinGW-w64)

1. Instala el toolchain de MinGW-w64:

   ```bash
   # Arch Linux
   sudo pacman -S mingw-w64-gcc

   # Debian / Ubuntu
   sudo apt install mingw-w64
   ```

2. Descarga los binarios de GLFW para MinGW desde la sección de releases de GLFW:
   <https://github.com/glfw/glfw/releases>

   Descomprime el archivo y coloca la carpeta renombrada como `deps/glfw-mingw` dentro del proyecto. La estructura debe quedar así:

   ```
   deps/glfw-mingw/
   ├── include/GLFW/
   │   ├── glfw3.h
   │   └── glfw3native.h
   └── lib-mingw-w64/
       ├── libglfw3.a
       ├── libglfw3dll.a
       └── glfw3.dll
   ```

3. Compila con el toolchain file incluido:

   ```bash
   cmake -S . -B build-win \
       -DCMAKE_TOOLCHAIN_FILE=mingw-w64-toolchain.cmake \
       -DGLFW_ROOT=$PWD/deps/glfw-mingw \
       -DCMAKE_BUILD_TYPE=Release
   cmake --build build-win -j
   ```

4. El `.exe` estará en:

   ```
   build-win/whip_simulator.exe
   ```

5. Para ejecutarlo en Windows, copia junto al `.exe`:
   - `glfw3.dll` (de `deps/glfw-mingw/lib-mingw-w64/`)
   - Las DLLs de runtime de MinGW (localízalas con `x86_64-w64-mingw32-g++ -print-file-name=libgcc_s_seh-1.dll` y similares):
     - `libgcc_s_seh-1.dll`
     - `libstdc++-6.dll`
     - `libwinpthread-1.dll`

---

## Ejecución

1. Coloca el archivo de sonido **`crack.wav`** en la misma carpeta desde la que ejecutes el binario.
2. Ejecuta el programa.

### Linux

```bash
./build/whip_simulator
```

### Windows

```cmd
.\build\Release\whip_simulator.exe
```

Si no tienes un `crack.wav`, el programa genera automáticamente uno de prueba al arrancar.

---

## Controles

- **Mover el ratón / touchpad**: el mango sigue al cursor.
- **Clic izquierdo mantenido**: agarras el mango y puedes azotar el látigo.
- **Mover bruscamente**: si la punta supera la velocidad umbral, suena el chasquido.

---

## Notas por plataforma

### Hyprland / Wayland

- El programa crea una **capa OVERLAY** a pantalla completa mediante `zwlr-layer-shell-v1`.
- No necesitas reglas de `windowrule` en `hyprland.conf`; la capa se dibuja encima de todo por diseño.
- La capa recibe input del ratón en toda la pantalla, por lo que cualquier clic interactúa con el látigo.

### Windows 11

- El programa crea una **ventana sin bordes y transparente** que se mantiene siempre encima gracias a `WS_EX_TOPMOST`.
- Funciona como cualquier otra ventana del escritorio.
- El audio usa la API nativa `PlaySound()` de Windows (`winmm.lib`).

---

## Estructura del proyecto

```
.
├── main.cpp                              # Código fuente principal
├── CMakeLists.txt                        # Configuración de compilación
├── wlr-layer-shell-unstable-v1.xml       # Protocolo Wayland layer-shell
├── wlr-layer-shell-unstable-v1-client-protocol.h  # Generado por CMake
├── wlr-layer-shell-unstable-v1-protocol.c         # Generado por CMake
├── .gitignore
└── crack.wav                             # Archivo de sonido (no versionado por defecto)
```

---

## Solución de problemas

### No se escucha el sonido en Linux

Asegúrate de tener instalado un reproductor WAV compatible:

```bash
which pw-play paplay aplay
```

El programa intenta usar `pw-play` → `paplay` → `aplay` en ese orden.

### En Windows la ventana no es transparente

Verifica que tu driver GPU soporte composición DWM con alpha por píxel. En Windows 10/11 moderno debería funcionar por defecto.

### Error de compilación relacionado con Wayland

Asegúrate de tener instalados `wayland-scanner` y los paquetes de desarrollo de `wayland-client`, `wayland-egl` y `egl`.

---

## Licencia

Proporcionado tal cual para uso personal y educativo.
