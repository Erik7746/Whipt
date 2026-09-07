cmake_minimum_required(VERSION 3.16)

# ============================================================================
#  LÁTIGO INTERACTIVO  (C++ / GLFW / OpenGL)
#  Proyecto multiplataforma: Windows 10/11 y Linux (Wayland/Hyprland).
# ============================================================================

project(whip_simulator LANGUAGES CXX)

# ---- Estándar de C++ ------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- Dependencias ---------------------------------------------------------
# GLFW (>= 3.3 para GLFW_TRANSPARENT_FRAMEBUFFER). El paquete CMake de GLFW
# expone el objetivo "glfw". En Debian/Ubuntu:  sudo apt install libglfw3-dev
find_package(glfw3 3.3 REQUIRED)

# OpenGL (para los comandos de renderizado en modo inmediato).
find_package(OpenGL REQUIRED)

# ---- Ejecutable -----------------------------------------------------------
add_executable(whip_simulator main.cpp)

# ---- Vinculación ----------------------------------------------------------
target_link_libraries(whip_simulator PRIVATE glfw OpenGL::GL)

# En Windows se necesita la librería winmm para PlaySound().
if(WIN32)
    target_link_libraries(whip_simulator PRIVATE winmm)
endif()

# Opcional: definición útil para depuración (no imprescindible).
if(MSVC)
    target_compile_options(whip_simulator PRIVATE /W4)
else()
    target_compile_options(whip_simulator PRIVATE -Wall -Wextra)
endif()

# ============================================================================
#  INSTRUCCIONES DE COMPILACIÓN
# ============================================================================
#  Linux (incluye Hyprland/Wayland):
#    cmake -S . -B build && cmake --build build -j
#    ./build/whip_simulator
#
#  Windows (PowerShell / cmd, con CMake y un generador como Visual Studio):
#    cmake -S . -B build
#    cmake --build build --config Release
#    .\build\Release\whip_simulator.exe
#
#  Coloca "crack.wav" en el mismo directorio desde el que ejecutas el binario.
# ============================================================================
