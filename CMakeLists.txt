cmake_minimum_required(VERSION 3.16)

# ============================================================================
#  LÁTIGO INTERACTIVO  (C++ / OpenGL)
#  Linux/Hyprland: usa Wayland layer-shell (capa OVERLAY real).
#  Windows       : usa GLFW con ventana transparente nativa.
# ============================================================================

project(whip_simulator LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================================
#  Linux / Wayland / Hyprland
# ============================================================================
if(NOT WIN32)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WAYLAND REQUIRED wayland-client wayland-egl wayland-cursor egl)
    pkg_check_modules(OPENGL REQUIRED gl)

    # Generar bindings del protocolo wlr-layer-shell-unstable-v1.
    # El protocolo se empaqueta en el proyecto para no depender de wlr-protocols.
    find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

    add_custom_command(
        OUTPUT
            ${CMAKE_CURRENT_BINARY_DIR}/wlr-layer-shell-unstable-v1-client-protocol.h
            ${CMAKE_CURRENT_BINARY_DIR}/wlr-layer-shell-unstable-v1-protocol.c
        COMMAND ${WAYLAND_SCANNER} client-header
            ${CMAKE_CURRENT_SOURCE_DIR}/wlr-layer-shell-unstable-v1.xml
            ${CMAKE_CURRENT_BINARY_DIR}/wlr-layer-shell-unstable-v1-client-protocol.h
        COMMAND ${WAYLAND_SCANNER} private-code
            ${CMAKE_CURRENT_SOURCE_DIR}/wlr-layer-shell-unstable-v1.xml
            ${CMAKE_CURRENT_BINARY_DIR}/wlr-layer-shell-unstable-v1-protocol.c
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/wlr-layer-shell-unstable-v1.xml
        COMMENT "Generando bindings de Wayland layer-shell"
    )

    add_executable(whip_simulator
        main.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/wlr-layer-shell-unstable-v1-protocol.c
    )

    target_include_directories(whip_simulator PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}
        ${WAYLAND_INCLUDE_DIRS}
        ${OPENGL_INCLUDE_DIRS}
    )

    target_link_libraries(whip_simulator PRIVATE
        ${WAYLAND_LIBRARIES}
        ${OPENGL_LIBRARIES}
    )

    target_compile_options(whip_simulator PRIVATE
        ${WAYLAND_CFLAGS_OTHER}
        ${OPENGL_CFLAGS_OTHER}
        -Wall -Wextra
    )

# ============================================================================
#  Windows (GLFW)
# ============================================================================
else()
    add_executable(whip_simulator main.cpp)

    # Permitir especificar una ruta custom de GLFW (útil para cross-compilar
    # con MinGW desde Linux usando los binarios precompilados de GLFW).
    if(DEFINED GLFW_ROOT)
        target_include_directories(whip_simulator PRIVATE ${GLFW_ROOT}/include)

        # Buscar la biblioteca estática en las ubicaciones típicas de los
        # paquetes binarios de GLFW para MinGW / MSVC.
        find_library(GLFW_LIBRARY
            NAMES glfw3 glfw3dll libglfw3.a libglfw3dll.a
            PATHS
                ${GLFW_ROOT}/lib-mingw-w64
                ${GLFW_ROOT}/lib-vc2022
                ${GLFW_ROOT}/lib-vc2019
                ${GLFW_ROOT}/lib
            NO_DEFAULT_PATH
            REQUIRED
        )
        target_link_libraries(whip_simulator PRIVATE ${GLFW_LIBRARY})
    else()
        find_package(glfw3 3.3 REQUIRED)
        target_link_libraries(whip_simulator PRIVATE glfw)
    endif()

    # En Windows nativo o cross-compilado con MinGW, OpenGL se llama opengl32.
    target_link_libraries(whip_simulator PRIVATE opengl32 winmm)

    if(MSVC)
        target_compile_options(whip_simulator PRIVATE /W4)
    else()
        target_compile_options(whip_simulator PRIVATE -Wall -Wextra)
    endif()
endif()

# ============================================================================
#  INSTRUCCIONES DE COMPILACIÓN
# ============================================================================
#  Linux / Hyprland:
#    cmake -S . -B build && cmake --build build -j
#    ./build/whip_simulator
#
#  Windows:
#    cmake -S . -B build
#    cmake --build build --config Release
#    .\build\Release\whip_simulator.exe
#
#  Coloca "crack.wav" en el directorio desde el que ejecutas el binario.
# ============================================================================
