/*
 * ============================================================================
 *  LÁTIGO INTERACTIVO EN TIEMPO REAL  (C++ / OpenGL)
 * ============================================================================
 *  Simula un látigo mediante una cadena de nodos con física de Verlet.
 *
 *  Plataformas:
 *    - Linux / Hyprland : usa Wayland layer-shell (capa OVERLAY real).
 *    - Windows 10/11    : usa GLFW con ventana transparente y estilos nativos.
 *
 *  El archivo de sonido "crack.wav" debe estar en el directorio de trabajo.
 * ============================================================================
 */

// ------------------------------- Plataforma ---------------------------------
#ifdef _WIN32
    #define USE_GLFW
#else
    #define USE_WAYLAND_LAYER
#endif

// ------------------------------- Includes -----------------------------------
#include <vector>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <string>
#include <fstream>
#include <cstdint>
#include <filesystem>

#ifdef USE_GLFW
    #include <GLFW/glfw3.h>
    #ifdef _WIN32
        #include <windows.h>
        #include <mmsystem.h>
        #pragma comment(lib, "winmm.lib")
    #else
        #include <cstdlib>
        #include <unistd.h>
        #include <fcntl.h>
    #endif
#endif

#ifdef USE_WAYLAND_LAYER
    #include <wayland-client.h>
    #include <wayland-egl.h>
    #include <wayland-cursor.h>
    #include <EGL/egl.h>
    #include <GL/gl.h>
    #include <cstdlib>
    #include <unistd.h>
    #include <fcntl.h>
    #include <cstring>
    #include <ctime>
    #include "wlr-layer-shell-unstable-v1-client-protocol.h"

    // Stub: el protocolo generado referencia xdg_popup_interface por el
    // request get_popup, pero nunca lo usamos.
    extern "C" const struct wl_interface xdg_popup_interface = {
        "xdg_popup", 3, 0, nullptr, 0, nullptr
    };
#endif

// ---------------------------------------------------------------------------
//  PARÁMETROS DEL MANGO
// ---------------------------------------------------------------------------
static const float HANDLE_LEN      = 55.0f;
static const float HANDLE_WIDTH    = 14.0f;
static const float HANDLE_ANGLE    = -75.0f * 3.14159265f / 180.0f;
static const float HANDLE_CORNER_R =  5.0f;

// ---------------------------------------------------------------------------
//  PARÁMETROS DE LA SIMULACIÓN FÍSICA
// ---------------------------------------------------------------------------
static const int   NUM_SEGMENTS          = 45;
static const int   NUM_NODES             = NUM_SEGMENTS + 1;
static const float SEGMENT_LENGTH_FLEX   = 9.0f;
static const float SEGMENT_LENGTH_RIGID  = 3.5f;
static const int   RIGID_SEGMENTS        = 8;
static const float GRAVITY               = 4500.0f;
static const float DAMPING               = 0.985f;
static const float DAMPING_RIGID         = 0.92f;
static const int   CONSTRAINT_ITERATIONS = 18;

static inline float getSegmentLength(int segIndex) {
    return (segIndex < RIGID_SEGMENTS) ? SEGMENT_LENGTH_RIGID : SEGMENT_LENGTH_FLEX;
}

// ---------------------------------------------------------------------------
//  PARÁMETROS DEL "CHASQUIDO"
// ---------------------------------------------------------------------------
static const float CRACK_THRESHOLD = 3500.0f;
static const float CRACK_COOLDOWN  = 0.25f;

// ---------------------------------------------------------------------------
//  Estructura de un nodo.
// ---------------------------------------------------------------------------
struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

// ---------------------------------------------------------------------------
//  Estado global compartido (tamaño de pantalla / ratón).
// ---------------------------------------------------------------------------
static int   g_screenW     = 1920;
static int   g_screenH     = 1080;
static float g_mouseX      = 0.0f;
static float g_mouseY      = 0.0f;
static bool  g_dragging    = false;

// Posiciones más recientes del látigo y del mango (para detección de clicks).
static std::vector<Vec2> g_whipPos;
static Vec2              g_handleGrip;

// ---------------------------------------------------------------------------
//  GENERADOR DE WAV DE PRUEBA
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct WavHeader {
    char     riff[4]       = {'R','I','F','F'};
    uint32_t fileSize      = 0;
    char     wave[4]       = {'W','A','V','E'};
    char     fmtChunk[4]   = {'f','m','t',' '};
    uint32_t fmtSize       = 16;
    uint16_t audioFormat   = 1;
    uint16_t numChannels   = 1;
    uint32_t sampleRate    = 44100;
    uint32_t byteRate      = 44100 * 2;
    uint16_t blockAlign    = 2;
    uint16_t bitsPerSample = 16;
    char     dataChunk[4]  = {'d','a','t','a'};
    uint32_t dataSize      = 0;
};
#pragma pack(pop)

static void generateTestWav(const std::string& filename) {
    const uint32_t sampleRate = 44100;
    const float    duration   = 0.25f;
    const float    frequency  = 1200.0f;
    const uint32_t numSamples = static_cast<uint32_t>(sampleRate * duration);

    std::vector<int16_t> samples(numSamples);
    for (uint32_t i = 0; i < numSamples; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(sampleRate);
        float envelope = std::exp(-t * 20.0f);
        float value = std::sin(6.283185307f * frequency * t) * envelope;
        samples[i] = static_cast<int16_t>(value * 28000.0f);
    }

    WavHeader hdr;
    hdr.dataSize = numSamples * sizeof(int16_t);
    hdr.fileSize = sizeof(WavHeader) + hdr.dataSize - 8;

    std::ofstream out(filename, std::ios::binary);
    if (!out) return;
    out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    out.write(reinterpret_cast<const char*>(samples.data()), hdr.dataSize);
    out.close();
}

// ---------------------------------------------------------------------------
//  AUDIO
// ---------------------------------------------------------------------------
#ifndef _WIN32
static std::string g_linuxAudioPlayer;

static void initLinuxAudio() {
    if (std::system("which pw-play >/dev/null 2>&1") == 0) {
        g_linuxAudioPlayer = "pw-play";
    } else if (std::system("which paplay >/dev/null 2>&1") == 0) {
        g_linuxAudioPlayer = "paplay";
    } else if (std::system("which aplay >/dev/null 2>&1") == 0) {
        g_linuxAudioPlayer = "aplay";
    } else {
        std::fprintf(stderr, "Aviso: no se encontró pw-play, paplay ni aplay.\n");
        g_linuxAudioPlayer.clear();
    }
}
#endif

void playWhipSound() {
#ifdef _WIN32
    PlaySound(TEXT("crack.wav"), nullptr, SND_FILENAME | SND_ASYNC);
#else
    if (g_linuxAudioPlayer.empty()) return;

    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull != -1) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        if (g_linuxAudioPlayer == "aplay") {
            execlp(g_linuxAudioPlayer.c_str(), g_linuxAudioPlayer.c_str(),
                   "-q", "crack.wav", nullptr);
        } else {
            execlp(g_linuxAudioPlayer.c_str(), g_linuxAudioPlayer.c_str(),
                   "crack.wav", nullptr);
        }
        _exit(1);
    }
#endif
}

// ---------------------------------------------------------------------------
//  FÍSICA
// ---------------------------------------------------------------------------
static void initWhip(std::vector<Vec2>& pos, std::vector<Vec2>& prev) {
    pos.resize(NUM_NODES);
    prev.resize(NUM_NODES);

    float grip_x = static_cast<float>(g_screenW) * 0.5f;
    float grip_y = static_cast<float>(g_screenH) * 0.25f;

    pos[0].x = grip_x + HANDLE_LEN * std::cos(HANDLE_ANGLE);
    pos[0].y = grip_y + HANDLE_LEN * std::sin(HANDLE_ANGLE);

    float baseAngle = HANDLE_ANGLE + 1.57079633f;

    for (int i = 1; i < NUM_NODES; ++i) {
        float len = getSegmentLength(i - 1);
        float t = static_cast<float>(i) * 0.25f;
        float curveX = 18.0f * std::exp(-t);
        float localAngle = baseAngle + (1.57079633f - baseAngle) *
                           (1.0f - std::exp(-static_cast<float>(i) * 0.18f));

        pos[i].x = pos[i - 1].x + std::cos(localAngle) * len + curveX * 0.08f;
        pos[i].y = pos[i - 1].y + std::sin(localAngle) * len;
    }

    prev = pos;
}

static void verletStep(std::vector<Vec2>& pos,
                       std::vector<Vec2>& prev,
                       float dt) {
    const float dt2 = dt * dt;

    for (int i = 1; i < NUM_NODES; ++i) {
        float damping = (i <= RIGID_SEGMENTS) ? DAMPING_RIGID : DAMPING;
        float vx = (pos[i].x - prev[i].x) * damping;
        float vy = (pos[i].y - prev[i].y) * damping;

        prev[i] = pos[i];
        pos[i].x += vx;
        pos[i].y += vy + GRAVITY * dt2;
    }
}

static void constrainDistance(Vec2& a, Vec2& b, float restLength) {
    float dx = b.x - a.x;
    float dy = b.y - a.y;
    float dist = std::sqrt(dx * dx + dy * dy);

    if (dist < 1e-4f) dist = 1e-4f;

    float correction = (dist - restLength) / dist;
    float ox = dx * 0.5f * correction;
    float oy = dy * 0.5f * correction;

    a.x += ox; a.y += oy;
    b.x -= ox; b.y -= oy;
}

static void solveConstraints(std::vector<Vec2>& pos) {
    for (int iter = 0; iter < CONSTRAINT_ITERATIONS; ++iter) {
        for (int i = 0; i < NUM_NODES - 1; ++i) {
            constrainDistance(pos[i], pos[i + 1], getSegmentLength(i));
        }
    }
}

static void updatePhysics(std::vector<Vec2>& pos,
                          std::vector<Vec2>& prev,
                          const Vec2& mouse,
                          bool dragging,
                          float dt) {
    if (dragging) {
        Vec2 tip;
        tip.x = mouse.x + HANDLE_LEN * std::cos(HANDLE_ANGLE);
        tip.y = mouse.y + HANDLE_LEN * std::sin(HANDLE_ANGLE);
        prev[0] = tip;
        pos[0]  = tip;
    } else {
        prev[0] = pos[0];
    }

    verletStep(pos, prev, dt);
    solveConstraints(pos);

    if (dragging) {
        pos[0].x = mouse.x + HANDLE_LEN * std::cos(HANDLE_ANGLE);
        pos[0].y = mouse.y + HANDLE_LEN * std::sin(HANDLE_ANGLE);
    } else {
        pos[0] = prev[0];
    }
}

static void detectCrack(const std::vector<Vec2>& pos,
                        const std::vector<Vec2>& prev,
                        float dt,
                        float& cooldownTimer,
                        bool& armed) {
    const Vec2& tip = pos[NUM_NODES - 1];
    const Vec2& tipPrev = prev[NUM_NODES - 1];

    float vx = (tip.x - tipPrev.x) / dt;
    float vy = (tip.y - tipPrev.y) / dt;
    float speed = std::sqrt(vx * vx + vy * vy);

    cooldownTimer -= dt;

    if (!armed && speed < CRACK_THRESHOLD * 0.5f) {
        armed = true;
    }

    if (armed && speed >= CRACK_THRESHOLD && cooldownTimer <= 0.0f) {
        playWhipSound();
        cooldownTimer = CRACK_COOLDOWN;
        armed = false;
    }
}

// ---------------------------------------------------------------------------
//  RENDERIZADO
// ---------------------------------------------------------------------------
static void renderWhip(const std::vector<Vec2>& pos) {
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glLineWidth(6.0f);
    glBegin(GL_LINE_STRIP);
    for (int i = 0; i < NUM_NODES; ++i) {
        glVertex2f(pos[i].x, pos[i].y);
    }
    glEnd();

    glColor4f(0.0f, 0.0f, 0.0f, 1.0f);
    glLineWidth(4.0f);
    glBegin(GL_LINE_STRIP);
    for (int i = 0; i < NUM_NODES; ++i) {
        glVertex2f(pos[i].x, pos[i].y);
    }
    glEnd();
}

static void renderHandle(const Vec2& grip) {
    static const int ARC_SEGS = 6;

    float L = HANDLE_LEN;
    float W = HANDLE_WIDTH;
    float r = std::min(HANDLE_CORNER_R, std::min(W * 0.5f, L * 0.5f));

    float ca = std::cos(HANDLE_ANGLE);
    float sa = std::sin(HANDLE_ANGLE);

    auto toWorld = [&](float lx, float ly) -> Vec2 {
        Vec2 v;
        v.x = grip.x + (lx * ca - ly * sa);
        v.y = grip.y + (lx * sa + ly * ca);
        return v;
    };

    std::vector<Vec2> outline;

    for (int i = 0; i <= ARC_SEGS; ++i) {
        float t = static_cast<float>(i) / ARC_SEGS;
        float a = 3.14159265f - t * 1.57079633f;
        outline.push_back(toWorld(r + r * std::cos(a),
                                  (W * 0.5f - r) + r * std::sin(a)));
    }

    outline.push_back(toWorld(L - r, W * 0.5f));

    for (int i = 0; i <= ARC_SEGS; ++i) {
        float t = static_cast<float>(i) / ARC_SEGS;
        float a = 1.57079633f * (1.0f - t);
        outline.push_back(toWorld((L - r) + r * std::cos(a),
                                  (W * 0.5f - r) + r * std::sin(a)));
    }

    outline.push_back(toWorld(L, W * 0.5f - r));
    outline.push_back(toWorld(L, -(W * 0.5f - r)));

    for (int i = 0; i <= ARC_SEGS; ++i) {
        float t = static_cast<float>(i) / ARC_SEGS;
        float a = -1.57079633f * t;
        outline.push_back(toWorld((L - r) + r * std::cos(a),
                                  -(W * 0.5f - r) + r * std::sin(a)));
    }

    outline.push_back(toWorld(r, -W * 0.5f));

    for (int i = 0; i <= ARC_SEGS; ++i) {
        float t = static_cast<float>(i) / ARC_SEGS;
        float a = -1.57079633f - 1.57079633f * t;
        outline.push_back(toWorld(r + r * std::cos(a),
                                  -(W * 0.5f - r) + r * std::sin(a)));
    }

    glColor4f(0.0f, 0.0f, 0.0f, 1.0f);
    glBegin(GL_POLYGON);
    for (const auto& v : outline) {
        glVertex2f(v.x, v.y);
    }
    glEnd();

    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glLineWidth(1.5f);
    glBegin(GL_LINE_LOOP);
    for (const auto& v : outline) {
        glVertex2f(v.x, v.y);
    }
    glEnd();
}

// ---------------------------------------------------------------------------
//  DETECCIÓN DE CLICK SOBRE EL OBJETO
//  El látigo solo se agarra si se pulsa directamente sobre él o sobre el mango.
// ---------------------------------------------------------------------------
static float pointSegmentDistance(float px, float py,
                                  float ax, float ay,
                                  float bx, float by) {
    float abx = bx - ax;
    float aby = by - ay;
    float apx = px - ax;
    float apy = py - ay;
    float ab2 = abx * abx + aby * aby;
    float t = (ab2 > 1e-8f) ? (apx * abx + apy * aby) / ab2 : 0.0f;
    t = std::max(0.0f, std::min(1.0f, t));
    float cx = ax + t * abx;
    float cy = ay + t * aby;
    float dx = px - cx;
    float dy = py - cy;
    return std::sqrt(dx * dx + dy * dy);
}

static bool isMouseOverObject(float x, float y) {
    if (g_whipPos.empty()) return false;

    // Distancia al látigo (umbral generoso para facilitar el agarre).
    for (size_t i = 0; i + 1 < g_whipPos.size(); ++i) {
        float d = pointSegmentDistance(x, y,
                                       g_whipPos[i].x, g_whipPos[i].y,
                                       g_whipPos[i + 1].x, g_whipPos[i + 1].y);
        if (d < 28.0f) return true;
    }

    // Distancia al eje del mango.
    Vec2 tip;
    tip.x = g_handleGrip.x + HANDLE_LEN * std::cos(HANDLE_ANGLE);
    tip.y = g_handleGrip.y + HANDLE_LEN * std::sin(HANDLE_ANGLE);
    float dHandle = pointSegmentDistance(x, y,
                                         g_handleGrip.x, g_handleGrip.y,
                                         tip.x, tip.y);
    if (dHandle < (HANDLE_WIDTH * 0.5f + 18.0f)) return true;

    return false;
}

// ---------------------------------------------------------------------------
//  CONFIGURACIÓN COMÚN DE OPENGL
// ---------------------------------------------------------------------------
static void setup_gl_state() {
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_LINE_SMOOTH);
    glEnable(GL_MULTISAMPLE);
    glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, g_screenW, g_screenH, 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

// ===========================================================================
//  PLATAFORMA: WAYLAND LAYER-SHELL (Linux / Hyprland)
// ===========================================================================
#ifdef USE_WAYLAND_LAYER

struct WaylandState {
    wl_display*     display         = nullptr;
    wl_registry*    registry        = nullptr;
    wl_compositor*  compositor      = nullptr;
    wl_shm*         shm             = nullptr;
    wl_output*      output          = nullptr;
    wl_seat*        seat            = nullptr;
    wl_pointer*     pointer         = nullptr;
    wl_surface*     surface         = nullptr;
    wl_surface*     cursor_surface  = nullptr;
    wl_egl_window*  egl_window      = nullptr;
    wl_cursor_theme* cursor_theme   = nullptr;
    wl_cursor*      cursor          = nullptr;
    zwlr_layer_shell_v1*       layer_shell   = nullptr;
    zwlr_layer_surface_v1*     layer_surface = nullptr;

    EGLDisplay      egl_display     = EGL_NO_DISPLAY;
    EGLContext      egl_context     = EGL_NO_CONTEXT;
    EGLSurface      egl_surface     = EGL_NO_SURFACE;

    int             pending_w       = 0;
    int             pending_h       = 0;
    bool            configured      = false;
    bool            closed          = false;
    uint32_t        configure_serial = 0;
    uint32_t        pointer_serial  = 0;
};

static WaylandState g_wl;

// ---- Registry listener ----------------------------------------------------
static void registry_global(void* data, wl_registry* registry,
                            uint32_t id, const char* interface, uint32_t version) {
    (void)version;
    WaylandState* s = static_cast<WaylandState*>(data);
    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
        s->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, id, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
        s->shm = static_cast<wl_shm*>(
            wl_registry_bind(registry, id, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, wl_output_interface.name) == 0) {
        if (!s->output) {
            s->output = static_cast<wl_output*>(
                wl_registry_bind(registry, id, &wl_output_interface, 2));
        }
    } else if (std::strcmp(interface, wl_seat_interface.name) == 0) {
        s->seat = static_cast<wl_seat*>(
            wl_registry_bind(registry, id, &wl_seat_interface, 5));
    } else if (std::strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        s->layer_shell = static_cast<zwlr_layer_shell_v1*>(
            wl_registry_bind(registry, id, &zwlr_layer_shell_v1_interface, 4));
    }
}

static void registry_global_remove(void*, wl_registry*, uint32_t) {}

static const wl_registry_listener registry_listener = {
    registry_global,
    registry_global_remove
};

// ---- Layer surface listener -----------------------------------------------
static void layer_surface_configure(void* data,
                                    zwlr_layer_surface_v1* /*surface*/,
                                    uint32_t serial,
                                    uint32_t width,
                                    uint32_t height) {
    WaylandState* s = static_cast<WaylandState*>(data);
    s->pending_w = static_cast<int>(width);
    s->pending_h = static_cast<int>(height);
    s->configure_serial = serial;
    s->configured = true;
}

static void layer_surface_closed(void* data,
                                 zwlr_layer_surface_v1* /*surface*/) {
    WaylandState* s = static_cast<WaylandState*>(data);
    s->closed = true;
}

static const zwlr_layer_surface_v1_listener layer_surface_listener = {
    layer_surface_configure,
    layer_surface_closed
};

// ---- Pointer listener -----------------------------------------------------
static void set_wayland_cursor(WaylandState* s, uint32_t serial) {
    if (!s->cursor || !s->cursor_surface || s->cursor->image_count == 0) return;
    wl_cursor_image* image = s->cursor->images[0];
    wl_buffer* buffer = wl_cursor_image_get_buffer(image);
    if (!buffer) return;

    wl_pointer_set_cursor(s->pointer, serial, s->cursor_surface,
                          image->hotspot_x, image->hotspot_y);
    wl_surface_attach(s->cursor_surface, buffer, 0, 0);
    wl_surface_damage(s->cursor_surface, 0, 0, image->width, image->height);
    wl_surface_commit(s->cursor_surface);
}

static void pointer_enter(void* data,
                          wl_pointer* /*pointer*/,
                          uint32_t serial,
                          wl_surface* /*surface*/,
                          wl_fixed_t sx,
                          wl_fixed_t sy) {
    WaylandState* s = static_cast<WaylandState*>(data);
    s->pointer_serial = serial;
    g_mouseX = wl_fixed_to_double(sx);
    g_mouseY = wl_fixed_to_double(sy);
    set_wayland_cursor(s, serial);
}

static void pointer_leave(void* /*data*/,
                          wl_pointer* /*pointer*/,
                          uint32_t /*serial*/,
                          wl_surface* /*surface*/) {}

static void pointer_motion(void* data,
                           wl_pointer* /*pointer*/,
                           uint32_t /*time*/,
                           wl_fixed_t sx,
                           wl_fixed_t sy) {
    WaylandState* s = static_cast<WaylandState*>(data);
    g_mouseX = wl_fixed_to_double(sx);
    g_mouseY = wl_fixed_to_double(sy);
    // Reafirmar el cursor periódicamente por si el compositor lo pierde.
    set_wayland_cursor(s, s->pointer_serial);
}

static void pointer_button(void* /*data*/,
                           wl_pointer* /*pointer*/,
                           uint32_t /*serial*/,
                           uint32_t /*time*/,
                           uint32_t button,
                           uint32_t state) {
    if (button == 272) { // BTN_LEFT
        if (state == 1) {
            // Solo iniciar arrastre si el cursor está sobre el objeto.
            g_dragging = isMouseOverObject(g_mouseX, g_mouseY);
        } else {
            g_dragging = false;
        }
    }
}

static void pointer_axis(void* /*data*/,
                         wl_pointer* /*pointer*/,
                         uint32_t /*time*/,
                         uint32_t /*axis*/,
                         wl_fixed_t /*value*/) {}

static void pointer_frame(void* /*data*/, wl_pointer* /*pointer*/) {}

static void pointer_axis_source(void* /*data*/, wl_pointer* /*pointer*/,
                                uint32_t /*axis_source*/) {}

static void pointer_axis_stop(void* /*data*/, wl_pointer* /*pointer*/,
                              uint32_t /*time*/, uint32_t /*axis*/) {}

static void pointer_axis_discrete(void* /*data*/, wl_pointer* /*pointer*/,
                                  uint32_t /*axis*/, int32_t /*discrete*/) {}

static void pointer_axis_value120(void* /*data*/, wl_pointer* /*pointer*/,
                                  uint32_t /*axis*/, int32_t /*value120*/) {}

static void pointer_axis_relative_direction(void* /*data*/, wl_pointer* /*pointer*/,
                                            uint32_t /*axis*/, uint32_t /*direction*/) {}

static const wl_pointer_listener pointer_listener = {
    pointer_enter,
    pointer_leave,
    pointer_motion,
    pointer_button,
    pointer_axis,
    pointer_frame,
    pointer_axis_source,
    pointer_axis_stop,
    pointer_axis_discrete,
    pointer_axis_value120,
    pointer_axis_relative_direction
};

// ---- Seat listener --------------------------------------------------------
static void seat_capabilities(void* data, wl_seat* seat, uint32_t caps) {
    WaylandState* s = static_cast<WaylandState*>(data);
    if (caps & WL_SEAT_CAPABILITY_POINTER) {
        if (!s->pointer) {
            s->pointer = wl_seat_get_pointer(seat);
            wl_pointer_add_listener(s->pointer, &pointer_listener, s);
        }
    }
}

static void seat_name(void* /*data*/, wl_seat* /*seat*/, const char* /*name*/) {}

static const wl_seat_listener seat_listener = {
    seat_capabilities,
    seat_name
};

// ---- EGL helpers ----------------------------------------------------------
static bool init_egl(WaylandState* s, int width, int height) {
    eglBindAPI(EGL_OPENGL_API);

    s->egl_display = eglGetDisplay(s->display);
    if (s->egl_display == EGL_NO_DISPLAY) {
        std::fprintf(stderr, "Error: eglGetDisplay falló.\n");
        return false;
    }

    EGLint major, minor;
    if (!eglInitialize(s->egl_display, &major, &minor)) {
        std::fprintf(stderr, "Error: eglInitialize falló.\n");
        return false;
    }

    EGLint attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };

    EGLConfig config;
    EGLint num_configs;
    if (!eglChooseConfig(s->egl_display, attribs, &config, 1, &num_configs) ||
        num_configs == 0) {
        std::fprintf(stderr, "Error: eglChooseConfig falló.\n");
        return false;
    }

    EGLint ctx_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 2,
        EGL_CONTEXT_MINOR_VERSION, 1,
        EGL_NONE
    };

    s->egl_context = eglCreateContext(s->egl_display, config,
                                      EGL_NO_CONTEXT, ctx_attribs);
    if (s->egl_context == EGL_NO_CONTEXT) {
        std::fprintf(stderr, "Error: eglCreateContext falló.\n");
        return false;
    }

    s->egl_window = wl_egl_window_create(s->surface, width, height);
    if (!s->egl_window) {
        std::fprintf(stderr, "Error: wl_egl_window_create falló.\n");
        return false;
    }

    s->egl_surface = eglCreateWindowSurface(s->egl_display, config,
                                            s->egl_window, nullptr);
    if (s->egl_surface == EGL_NO_SURFACE) {
        std::fprintf(stderr, "Error: eglCreateWindowSurface falló.\n");
        return false;
    }

    eglMakeCurrent(s->egl_display, s->egl_surface, s->egl_surface,
                   s->egl_context);
    eglSwapInterval(s->egl_display, 1);

    return true;
}

// ---- Inicialización Wayland completa --------------------------------------
static bool init_wayland(WaylandState* s) {
    s->display = wl_display_connect(nullptr);
    if (!s->display) {
        std::fprintf(stderr, "Error: no se pudo conectar al display Wayland.\n");
        return false;
    }

    s->registry = wl_display_get_registry(s->display);
    wl_registry_add_listener(s->registry, &registry_listener, s);
    wl_display_roundtrip(s->display);

    if (!s->compositor || !s->layer_shell || !s->shm) {
        std::fprintf(stderr, "Error: faltan interfaces de Wayland necesarias.\n");
        return false;
    }

    if (s->seat) {
        wl_seat_add_listener(s->seat, &seat_listener, s);
    }

    // Cargar cursor por defecto.
    s->cursor_theme = wl_cursor_theme_load(nullptr, 24, s->shm);
    if (s->cursor_theme) {
        s->cursor = wl_cursor_theme_get_cursor(s->cursor_theme, "default");
        if (s->cursor) {
            s->cursor_surface = wl_compositor_create_surface(s->compositor);
        }
    }

    s->surface = wl_compositor_create_surface(s->compositor);

    s->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        s->layer_shell,
        s->surface,
        s->output,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
        "whip_overlay");

    zwlr_layer_surface_v1_set_size(s->layer_surface, 0, 0);
    zwlr_layer_surface_v1_set_anchor(s->layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(s->layer_surface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(s->layer_surface,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);

    zwlr_layer_surface_v1_add_listener(s->layer_surface,
                                       &layer_surface_listener, s);
    wl_surface_commit(s->surface);

    // Esperar configure
    while (!s->configured && !s->closed) {
        if (wl_display_dispatch(s->display) == -1) {
            std::fprintf(stderr, "Error: dispatch falló esperando configure.\n");
            return false;
        }
    }

    if (s->closed) {
        return false;
    }

    // Aplicar tamaño configurado.
    zwlr_layer_surface_v1_ack_configure(s->layer_surface, s->configure_serial);
    wl_surface_commit(s->surface);

    if (s->pending_w > 0 && s->pending_h > 0) {
        g_screenW = s->pending_w;
        g_screenH = s->pending_h;
    }

    if (!init_egl(s, g_screenW, g_screenH)) {
        return false;
    }

    return true;
}

static void cleanup_wayland(WaylandState* s) {
    if (s->egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(s->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
    }
    if (s->egl_surface != EGL_NO_SURFACE) {
        eglDestroySurface(s->egl_display, s->egl_surface);
    }
    if (s->egl_window) {
        wl_egl_window_destroy(s->egl_window);
    }
    if (s->egl_context != EGL_NO_CONTEXT) {
        eglDestroyContext(s->egl_display, s->egl_context);
    }
    if (s->egl_display != EGL_NO_DISPLAY) {
        eglTerminate(s->egl_display);
    }
    if (s->layer_surface) {
        zwlr_layer_surface_v1_destroy(s->layer_surface);
    }
    if (s->surface) {
        wl_surface_destroy(s->surface);
    }
    if (s->cursor_surface) {
        wl_surface_destroy(s->cursor_surface);
    }
    if (s->cursor_theme) {
        wl_cursor_theme_destroy(s->cursor_theme);
    }
    if (s->pointer) {
        wl_pointer_destroy(s->pointer);
    }
    if (s->seat) {
        wl_seat_destroy(s->seat);
    }
    if (s->output) {
        wl_output_destroy(s->output);
    }
    if (s->layer_shell) {
        zwlr_layer_shell_v1_destroy(s->layer_shell);
    }
    if (s->shm) {
        wl_shm_destroy(s->shm);
    }
    if (s->compositor) {
        wl_compositor_destroy(s->compositor);
    }
    if (s->registry) {
        wl_registry_destroy(s->registry);
    }
    if (s->display) {
        wl_display_disconnect(s->display);
    }
}

static int run_wayland() {
#ifndef _WIN32
    initLinuxAudio();
    // Generar un WAV de prueba silenciosamente si no existe uno propio.
    if (!std::filesystem::exists("crack.wav")) {
        generateTestWav("crack.wav");
    }
#endif

    if (!init_wayland(&g_wl)) {
        return -1;
    }

    setup_gl_state();

    std::vector<Vec2> pos, prev;
    initWhip(pos, prev);

    // Inicializar datos de hit-test antes de que lleguen eventos de ratón.
    g_whipPos = pos;
    g_handleGrip.x = pos[0].x - HANDLE_LEN * std::cos(HANDLE_ANGLE);
    g_handleGrip.y = pos[0].y - HANDLE_LEN * std::sin(HANDLE_ANGLE);

    bool  armed         = false;
    float crackCooldown = 0.0f;
    double lastTime     = 0.0;

    while (!g_wl.closed) {
        wl_display_dispatch_pending(g_wl.display);
        wl_display_flush(g_wl.display);

        double now = 0.0;
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
            now = ts.tv_sec + ts.tv_nsec * 1e-9;
        }
        float dt = static_cast<float>(now - lastTime);
        lastTime = now;
        if (dt > 0.05f) dt = 0.05f;
        if (dt < 0.001f) dt = 0.001f;

        Vec2 mouse = { g_mouseX, g_mouseY };
        updatePhysics(pos, prev, mouse, g_dragging, dt);
        detectCrack(pos, prev, dt, crackCooldown, armed);

        // Guardar posiciones actuales para la detección de clicks.
        g_whipPos = pos;

        glViewport(0, 0, g_screenW, g_screenH);
        glClear(GL_COLOR_BUFFER_BIT);

        renderWhip(pos);

        Vec2 grip;
        if (g_dragging) {
            grip = mouse;
        } else {
            grip.x = pos[0].x - HANDLE_LEN * std::cos(HANDLE_ANGLE);
            grip.y = pos[0].y - HANDLE_LEN * std::sin(HANDLE_ANGLE);
        }
        g_handleGrip = grip;
        renderHandle(grip);

        eglSwapBuffers(g_wl.egl_display, g_wl.egl_surface);
    }

    cleanup_wayland(&g_wl);
    return 0;
}

#endif // USE_WAYLAND_LAYER

// ===========================================================================
//  PLATAFORMA: GLFW (Windows)
// ===========================================================================
#ifdef USE_GLFW

static void mouseButtonCallback(GLFWwindow* /*window*/, int button, int action, int /*mods*/) {
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        if (action == GLFW_PRESS) {
            g_dragging = isMouseOverObject(g_mouseX, g_mouseY);
        } else {
            g_dragging = false;
        }
    }
}

static int run_glfw() {
    if (!glfwInit()) {
        std::fprintf(stderr, "Error: no se pudo inicializar GLFW.\n");
        return -1;
    }

    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_SAMPLES, 4);

    GLFWwindow* window = glfwCreateWindow(g_screenW, g_screenH,
                                          "whip_simulator", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "Error: no se pudo crear la ventana.\n");
        glfwTerminate();
        return -1;
    }

#ifdef _WIN32
    HWND hwnd = glfwGetWin32Window(window);
    if (hwnd) {
        LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
        exStyle |= WS_EX_LAYERED | WS_EX_TOPMOST;
        SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle);
    }
#endif

    glfwSetMouseButtonCallback(window, mouseButtonCallback);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    setup_gl_state();

    std::vector<Vec2> pos, prev;
    initWhip(pos, prev);

    // Inicializar datos de hit-test antes de que lleguen eventos de ratón.
    g_whipPos = pos;
    g_handleGrip.x = pos[0].x - HANDLE_LEN * std::cos(HANDLE_ANGLE);
    g_handleGrip.y = pos[0].y - HANDLE_LEN * std::sin(HANDLE_ANGLE);

    bool  armed         = false;
    float crackCooldown = 0.0f;
    double lastTime     = glfwGetTime();

    while (!glfwWindowShouldClose(window)) {
        double now = glfwGetTime();
        float dt = static_cast<float>(now - lastTime);
        lastTime = now;
        if (dt > 0.05f) dt = 0.05f;

        double mx, my;
        glfwGetCursorPos(window, &mx, &my);
        g_mouseX = static_cast<float>(mx);
        g_mouseY = static_cast<float>(my);
        Vec2 mouse = { g_mouseX, g_mouseY };

        updatePhysics(pos, prev, mouse, g_dragging, dt);
        detectCrack(pos, prev, dt, crackCooldown, armed);

        // Guardar posiciones actuales para la detección de clicks.
        g_whipPos = pos;

        glViewport(0, 0, g_screenW, g_screenH);
        glClear(GL_COLOR_BUFFER_BIT);

        renderWhip(pos);

        Vec2 grip;
        if (g_dragging) {
            grip = mouse;
        } else {
            grip.x = pos[0].x - HANDLE_LEN * std::cos(HANDLE_ANGLE);
            grip.y = pos[0].y - HANDLE_LEN * std::sin(HANDLE_ANGLE);
        }
        g_handleGrip = grip;
        renderHandle(grip);

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

#endif // USE_GLFW

// ---------------------------------------------------------------------------
//  PROGRAMA PRINCIPAL
// ---------------------------------------------------------------------------
int main() {
#ifdef USE_WAYLAND_LAYER
    return run_wayland();
#else
    return run_glfw();
#endif
}
