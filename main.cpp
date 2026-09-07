/*
 * ============================================================================
 *  LÁTIGO INTERACTIVO EN TIEMPO REAL  (C++ / GLFW / OpenGL)
 * ============================================================================
 *  Simula un látigo mediante una cadena de nodos con física de Verlet.
 *  La ventana es transparente y sin bordes: solo se ve el látigo.
 *
 *  Compatibilidad:  Windows 10/11 (DWM + WS_EX_LAYERED/TOPMOST)
 *                   Linux / Hyprland (Wayland) con transparencia nativa.
 *
 *  Compilación: véase CMakeLists.txt (proyecto "whip_simulator").
 *
 *  El archivo de sonido "crack.wav" debe estar en el directorio de trabajo.
 * ============================================================================
 */

// ------------------------------- Includes -----------------------------------
#include <GLFW/glfw3.h>

#ifdef _WIN32
    // API nativa de Windows: ventana en capas, siempre-encima y audio.
    #include <windows.h>
    #include <mmsystem.h>          // PlaySound()
    #pragma comment(lib, "winmm.lib")
#else
    // Linux: reproducir el WAV de forma asíncrona.
    #include <cstdlib>
    #include <unistd.h>    // fork(), execlp(), _exit()
    #include <fcntl.h>     // open(), O_WRONLY
#endif

#include <vector>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <string>
#include <fstream>
#include <cstdint>
#include <filesystem>

// ---------------------------------------------------------------------------
//  PARÁMETROS DE LA VENTANA
// ---------------------------------------------------------------------------
static const int WINDOW_WIDTH  = 900;
static const int WINDOW_HEIGHT = 700;

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
static const int   NUM_SEGMENTS          = 45;      // Segmentos totales
static const int   NUM_NODES             = NUM_SEGMENTS + 1;
static const float SEGMENT_LENGTH_FLEX   = 9.0f;    // Longitud del látigo flexible (px)
static const float SEGMENT_LENGTH_RIGID  = 3.5f;    // Longitud de la zona rígida cerca del mango (px)
static const int   RIGID_SEGMENTS        = 8;       // Cuántos segmentos al inicio son rígidos
static const float GRAVITY               = 4500.0f; // Aceleración hacia abajo (px/s^2)
static const float DAMPING               = 0.985f;  // Amortiguamiento general (menos rebote)
static const float DAMPING_RIGID         = 0.92f;   // Amortiguamiento extra en la zona rígida
static const int   CONSTRAINT_ITERATIONS = 18;      // Más iteraciones = más rigidez, menos elasticidad

// Longitud de reposo del segmento i.
static inline float getSegmentLength(int segIndex) {
    return (segIndex < RIGID_SEGMENTS) ? SEGMENT_LENGTH_RIGID : SEGMENT_LENGTH_FLEX;
}

// ---------------------------------------------------------------------------
//  PARÁMETROS DEL "CHASQUIDO" (AZOTE)
// ---------------------------------------------------------------------------
static const float CRACK_THRESHOLD = 3500.0f; // Velocidad mínima de la punta (px/s)
static const float CRACK_COOLDOWN  = 0.25f;   // Tiempo mínimo entre chasquidos (s)

// ---------------------------------------------------------------------------
//  Estructura de un nodo: posición actual y posición previa (Verlet).
//  La diferencia (pos - prev) es la velocidad implícita del nodo.
// ---------------------------------------------------------------------------
struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

// ---------------------------------------------------------------------------
//  GENERADOR DE WAV DE PRUEBA (PCM mono 44100 Hz, 16 bits)
//  Crea un archivo RIFF/WAV válido con un tono de "chasquido" de prueba
//  para que el usuario pueda verificar el audio sin depender de un archivo
//  externo. Se invoca automáticamente si "crack.wav" no existe.
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct WavHeader {
    char     riff[4]       = {'R','I','F','F'};
    uint32_t fileSize      = 0; // se rellena después.
    char     wave[4]       = {'W','A','V','E'};
    char     fmtChunk[4]   = {'f','m','t',' '};
    uint32_t fmtSize       = 16;
    uint16_t audioFormat   = 1; // PCM
    uint16_t numChannels   = 1; // Mono
    uint32_t sampleRate    = 44100;
    uint32_t byteRate      = 44100 * 2;
    uint16_t blockAlign    = 2;
    uint16_t bitsPerSample = 16;
    char     dataChunk[4]  = {'d','a','t','a'};
    uint32_t dataSize      = 0; // se rellena después.
};
#pragma pack(pop)

static void generateTestWav(const std::string& filename) {
    const uint32_t sampleRate = 44100;
    const float    duration   = 0.25f;   // 250 ms
    const float    frequency  = 1200.0f; // Tono agudo de prueba.
    const uint32_t numSamples = static_cast<uint32_t>(sampleRate * duration);

    std::vector<int16_t> samples(numSamples);
    for (uint32_t i = 0; i < numSamples; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(sampleRate);
        // Envoltura exponencial decreciente para que suene como "crack".
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
//  REPRODUCCIÓN DE AUDIO DEL CHASQUIDO
// ---------------------------------------------------------------------------
//  Carga y reproduce "crack.wav". Se llama de forma NO bloqueante:
//    - Windows : PlaySound con SND_ASYNC (no detiene el bucle).
//    - Linux   : detecta automáticamente pw-play / paplay / aplay.
// ---------------------------------------------------------------------------

#ifndef _WIN32
static std::string g_linuxAudioPlayer;   // Nombre del ejecutable detectado.

static void initLinuxAudio() {
    // Intenta encontrar un reproductor WAV disponible en el sistema.
    if (std::system("which pw-play >/dev/null 2>&1") == 0) {
        g_linuxAudioPlayer = "pw-play";
    } else if (std::system("which paplay >/dev/null 2>&1") == 0) {
        g_linuxAudioPlayer = "paplay";
    } else if (std::system("which aplay >/dev/null 2>&1") == 0) {
        g_linuxAudioPlayer = "aplay";
    } else {
        std::fprintf(stderr, "Aviso: no se encontró pw-play, paplay ni aplay. "
                             "El sonido del chasquido estará desactivado.\n");
        g_linuxAudioPlayer.clear();
    }
}
#endif

void playWhipSound() {
#ifdef _WIN32
    PlaySound(TEXT("crack.wav"), nullptr, SND_FILENAME | SND_ASYNC);
#else
    if (g_linuxAudioPlayer.empty()) return;

    // Lanzamos el reproductor mediante fork()+exec() en vez de system().
    // Esto es 100% asíncrono, no bloquea el hilo de renderizado y evita
    // problemas de job-control del shell con procesos en background.
    pid_t pid = fork();
    if (pid == 0) {
        // Proceso hijo: silenciamos stdout/stderr y ejecutamos el reproductor.
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull != -1) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        // Solo aplay (ALSA) acepta "-q" (quiet).
        // pw-play usa -q para "--quality" (¡calidad del resampler!), no para silencio.
        // paplay no tiene flag de quiet.
        if (g_linuxAudioPlayer == "aplay") {
            execlp(g_linuxAudioPlayer.c_str(), g_linuxAudioPlayer.c_str(),
                   "-q", "crack.wav", nullptr);
        } else {
            execlp(g_linuxAudioPlayer.c_str(), g_linuxAudioPlayer.c_str(),
                   "crack.wav", nullptr);
        }
        _exit(1); // Si execlp falla, terminamos el hijo inmediatamente.
    }
    // El padre ignora el pid y continúa el bucle de simulación sin esperar.
#endif
}

// ---------------------------------------------------------------------------
//  OBTENER POSICIÓN DEL CURSOR (coordenadas de ventana, origen arriba-izq.)
// ---------------------------------------------------------------------------
static Vec2 getMousePosition(GLFWwindow* window) {
    double mx = 0.0, my = 0.0;
    glfwGetCursorPos(window, &mx, &my);
    Vec2 p;
    p.x = static_cast<float>(mx);
    p.y = static_cast<float>(my);
    return p;
}

// ---------------------------------------------------------------------------
//  INICIALIZAR EL LÁTIGO
//  El nodo 0 es la PUNTA del mango. Se crea una zona rígida densa al inicio
//  y una curvatura natural en reposo hacia la derecha para simular la unión
//  mango-cuero de un látigo real.
// ---------------------------------------------------------------------------
static void initWhip(std::vector<Vec2>& pos, std::vector<Vec2>& prev) {
    pos.resize(NUM_NODES);
    prev.resize(NUM_NODES);

    // Grip inicial (centro de pantalla, un poco arriba).
    float grip_x = static_cast<float>(WINDOW_WIDTH)  * 0.5f;
    float grip_y = static_cast<float>(WINDOW_HEIGHT) * 0.25f;

    // La punta del mango (nodo 0) está adelantada según el ángulo fijo.
    pos[0].x = grip_x + HANDLE_LEN * std::cos(HANDLE_ANGLE);
    pos[0].y = grip_y + HANDLE_LEN * std::sin(HANDLE_ANGLE);

    // Dirección base del primer segmento: misma inclinación del mango
    // pero suavizada hacia la vertical progresivamente.
    float baseAngle = HANDLE_ANGLE + 1.57079633f; // perpendicular al mango (hacia abajo)

    for (int i = 1; i < NUM_NODES; ++i) {
        float len = getSegmentLength(i - 1);

        // Curvatura inicial: los primeros nodos se desvían hacia la derecha
        // siguiendo una exponencial decreciente. Esto crea la curva suave
        // característica de la unión mango-látigo en reposo.
        float t = static_cast<float>(i) * 0.25f;
        float curveX = 18.0f * std::exp(-t);

        // Ángulo local: empieza alineado con el mango y gira hacia vertical.
        float localAngle = baseAngle + (1.57079633f - baseAngle) *
                           (1.0f - std::exp(-static_cast<float>(i) * 0.18f));

        pos[i].x = pos[i - 1].x + std::cos(localAngle) * len + curveX * 0.08f;
        pos[i].y = pos[i - 1].y + std::sin(localAngle) * len;
    }

    // Sin velocidad inicial.
    prev = pos;
}

// ---------------------------------------------------------------------------
//  PASO DE INTEGRACIÓN VERLET (nodos libres, excluyendo el mango)
//  Los nodos cercanos al mango usan mayor amortiguamiento (más rígidos).
// ---------------------------------------------------------------------------
static void verletStep(std::vector<Vec2>& pos,
                       std::vector<Vec2>& prev,
                       float dt) {
    const float dt2 = dt * dt;

    for (int i = 1; i < NUM_NODES; ++i) {
        // Amortiguamiento variable: más rígido cerca del mango.
        float damping = (i <= RIGID_SEGMENTS) ? DAMPING_RIGID : DAMPING;

        // Velocidad implícita = (posición actual - posición previa).
        float vx = (pos[i].x - prev[i].x) * damping;
        float vy = (pos[i].y - prev[i].y) * damping;

        prev[i] = pos[i];                 // La posición actual pasa a ser la previa.

        pos[i].x += vx;                   // Inercia.
        pos[i].y += vy + GRAVITY * dt2;   // Inercia + gravedad.
    }
}

// ---------------------------------------------------------------------------
//  RESTRICCIÓN DE DISTANCIA RÍGIDA ENTRE DOS NODOS
//  Mantiene la longitud del eslabón aproximadamente constante, como cuero.
// ---------------------------------------------------------------------------
static void constrainDistance(Vec2& a, Vec2& b, float restLength) {
    float dx = b.x - a.x;
    float dy = b.y - a.y;
    float dist = std::sqrt(dx * dx + dy * dy);

    if (dist < 1e-4f) dist = 1e-4f;       // Evita división por cero.

    // Cantidad de corrección (positiva = estirar, negativa = comprimir).
    float correction = (dist - restLength) / dist;

    // Se reparte el ajuste a partes iguales entre ambos extremos.
    float ox = dx * 0.5f * correction;
    float oy = dy * 0.5f * correction;

    a.x += ox; a.y += oy;
    b.x -= ox; b.y -= oy;
}

// ---------------------------------------------------------------------------
//  RESOLVER TODAS LAS RESTRICCIONES (varias pasadas => mayor rigidez)
//  Cada segmento usa su propia longitud de reposo (rígida o flexible).
// ---------------------------------------------------------------------------
static void solveConstraints(std::vector<Vec2>& pos) {
    for (int iter = 0; iter < CONSTRAINT_ITERATIONS; ++iter) {
        for (int i = 0; i < NUM_NODES - 1; ++i) {
            constrainDistance(pos[i], pos[i + 1], getSegmentLength(i));
        }
    }
}

// ---------------------------------------------------------------------------
//  ACTUALIZACIÓN FÍSICA COMPLETA DE UN FRAME
// ---------------------------------------------------------------------------
static void updatePhysics(std::vector<Vec2>& pos,
                          std::vector<Vec2>& prev,
                          const Vec2& mouse,
                          bool dragging,
                          float dt) {
    // 1) Control del mango.
    //    El mouse sostiene el extremo TRASERO (grip). La punta (nodo 0)
    //    está desplazada por HANDLE_LEN en la dirección del ángulo fijo.
    if (dragging) {
        Vec2 tip;
        tip.x = mouse.x + HANDLE_LEN * std::cos(HANDLE_ANGLE);
        tip.y = mouse.y + HANDLE_LEN * std::sin(HANDLE_ANGLE);
        prev[0] = tip;
        pos[0]  = tip;
    } else {
        // Al soltar, la punta permanece fija.
        prev[0] = pos[0];
    }

    // 2) Integración de Verlet para los nodos suspendidos.
    verletStep(pos, prev, dt);

    // 3) Restricciones de longitud rígida.
    solveConstraints(pos);

    // 4) Re-fijar la punta del mango (las restricciones pudieron moverla).
    if (dragging) {
        pos[0].x = mouse.x + HANDLE_LEN * std::cos(HANDLE_ANGLE);
        pos[0].y = mouse.y + HANDLE_LEN * std::sin(HANDLE_ANGLE);
    } else {
        pos[0] = prev[0];
    }
}

// ---------------------------------------------------------------------------
//  DETECCIÓN DEL CHASQUIDO: mide la velocidad de la punta del látigo.
// ---------------------------------------------------------------------------
static void detectCrack(const std::vector<Vec2>& pos,
                        const std::vector<Vec2>& prev,
                        float dt,
                        float& cooldownTimer,
                        bool& armed) {
    const Vec2& tip = pos[NUM_NODES - 1];
    const Vec2& tipPrev = prev[NUM_NODES - 1];

    // Velocidad lineal de la punta en px/s.
    float vx = (tip.x - tipPrev.x) / dt;
    float vy = (tip.y - tipPrev.y) / dt;
    float speed = std::sqrt(vx * vx + vy * vy);

    cooldownTimer -= dt;

    // Rearmar: la velocidad debe caer por debajo de la mitad del umbral.
    if (!armed && speed < CRACK_THRESHOLD * 0.5f) {
        armed = true;
    }

    // Disparar el chasquido si supera el umbral y ya pasó el enfriamiento.
    if (armed && speed >= CRACK_THRESHOLD && cooldownTimer <= 0.0f) {
        playWhipSound();
        cooldownTimer = CRACK_COOLDOWN;
        armed = false;
    }
}

// ---------------------------------------------------------------------------
//  RENDERIZADO DEL LÁTIGO
//  Estilo: relleno NEGRO grueso con contorno BLANCO.
//  El grosor es mayor cerca del mango para simular la unión.
// ---------------------------------------------------------------------------
static void renderWhip(const std::vector<Vec2>& pos) {
    // --- Contorno blanco (línea más gruesa por debajo) -----------------------
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glLineWidth(6.0f);
    glBegin(GL_LINE_STRIP);
    for (int i = 0; i < NUM_NODES; ++i) {
        glVertex2f(pos[i].x, pos[i].y);
    }
    glEnd();

    // --- Relleno negro (línea más fina encima) -------------------------------
    glColor4f(0.0f, 0.0f, 0.0f, 1.0f);
    glLineWidth(4.0f);
    glBegin(GL_LINE_STRIP);
    for (int i = 0; i < NUM_NODES; ++i) {
        glVertex2f(pos[i].x, pos[i].y);
    }
    glEnd();
}

// ---------------------------------------------------------------------------
//  DIBUJAR EL MANGO (rectángulo largo con bordes redondeados, visto de lado)
//  El mango está rígido, inclinado 75° hacia arriba-derecha.
//  Se dibuja desde el punto de agarre (grip) hacia adelante.
//  Estilo: relleno NEGRO, borde BLANCO.
// ---------------------------------------------------------------------------
static void renderHandle(const Vec2& grip) {
    static const int ARC_SEGS = 6;

    float L = HANDLE_LEN;
    float W = HANDLE_WIDTH;
    float r = std::min(HANDLE_CORNER_R, std::min(W * 0.5f, L * 0.5f));

    float ca = std::cos(HANDLE_ANGLE);
    float sa = std::sin(HANDLE_ANGLE);

    // Transforma coordenadas locales (origen = grip, +X = adelante)
    auto toWorld = [&](float lx, float ly) -> Vec2 {
        Vec2 v;
        v.x = grip.x + (lx * ca - ly * sa);
        v.y = grip.y + (lx * sa + ly * ca);
        return v;
    };

    std::vector<Vec2> outline;

    // 1) Esquina delantera-superior. Arco PI -> PI/2
    for (int i = 0; i <= ARC_SEGS; ++i) {
        float t = static_cast<float>(i) / ARC_SEGS;
        float a = 3.14159265f - t * 1.57079633f;
        outline.push_back(toWorld(r + r * std::cos(a),
                                  (W * 0.5f - r) + r * std::sin(a)));
    }

    // 2) Arista superior recta.
    outline.push_back(toWorld(L - r, W * 0.5f));

    // 3) Esquina trasera-superior. Arco PI/2 -> 0
    for (int i = 0; i <= ARC_SEGS; ++i) {
        float t = static_cast<float>(i) / ARC_SEGS;
        float a = 1.57079633f * (1.0f - t);
        outline.push_back(toWorld((L - r) + r * std::cos(a),
                                  (W * 0.5f - r) + r * std::sin(a)));
    }

    // 4) Arista trasera recta (completa).
    outline.push_back(toWorld(L, W * 0.5f - r));
    outline.push_back(toWorld(L, -(W * 0.5f - r)));

    // 5) Esquina trasera-inferior. Arco 0 -> -PI/2
    for (int i = 0; i <= ARC_SEGS; ++i) {
        float t = static_cast<float>(i) / ARC_SEGS;
        float a = -1.57079633f * t;
        outline.push_back(toWorld((L - r) + r * std::cos(a),
                                  -(W * 0.5f - r) + r * std::sin(a)));
    }

    // 6) Arista inferior recta.
    outline.push_back(toWorld(r, -W * 0.5f));

    // 7) Esquina delantera-inferior. Arco -PI/2 -> -PI
    for (int i = 0; i <= ARC_SEGS; ++i) {
        float t = static_cast<float>(i) / ARC_SEGS;
        float a = -1.57079633f - 1.57079633f * t;
        outline.push_back(toWorld(r + r * std::cos(a),
                                  -(W * 0.5f - r) + r * std::sin(a)));
    }

    // Relleno negro.
    glColor4f(0.0f, 0.0f, 0.0f, 1.0f);
    glBegin(GL_POLYGON);
    for (const auto& v : outline) {
        glVertex2f(v.x, v.y);
    }
    glEnd();

    // Borde blanco.
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glLineWidth(1.5f);
    glBegin(GL_LINE_LOOP);
    for (const auto& v : outline) {
        glVertex2f(v.x, v.y);
    }
    glEnd();
}

// ---------------------------------------------------------------------------
//  CALLBACK: detecta pulsación/soltura del botón izquierdo del ratón.
// ---------------------------------------------------------------------------
static void mouseButtonCallback(GLFWwindow* window, int button, int action, int /*mods*/) {
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        bool dragging = (action == GLFW_PRESS);
        glfwSetWindowUserPointer(window, reinterpret_cast<void*>(dragging));
    }
}

// ---------------------------------------------------------------------------
//  PROGRAMA PRINCIPAL
// ---------------------------------------------------------------------------
int main() {
    // ---- Inicializar GLFW ---------------------------------------------------
    if (!glfwInit()) {
        std::fprintf(stderr, "Error: no se pudo inicializar GLFW.\n");
        return -1;
    }

    // ---- Detectar reproductor de audio en Linux -----------------------------
#ifndef _WIN32
    initLinuxAudio();

    // Diagnóstico de audio: mostrar directorio actual y estado del archivo.
    {
        std::string cwd = std::filesystem::current_path().string();
        std::fprintf(stderr, "[Audio] Directorio de trabajo: %s\n", cwd.c_str());

        if (g_linuxAudioPlayer.empty()) {
            std::fprintf(stderr, "[Audio] Reproductor: NINGUNO (sonido desactivado)\n");
        } else {
            std::fprintf(stderr, "[Audio] Reproductor detectado: %s\n", g_linuxAudioPlayer.c_str());
        }

        if (!std::filesystem::exists("crack.wav")) {
            std::fprintf(stderr, "[Audio] crack.wav NO encontrado. Generando WAV de prueba...\n");
            generateTestWav("crack.wav");
            if (std::filesystem::exists("crack.wav")) {
                std::fprintf(stderr, "[Audio] crack.wav generado correctamente (tono de prueba).\n");
            } else {
                std::fprintf(stderr, "[Audio] ERROR: no se pudo generar crack.wav.\n");
            }
        } else {
            std::fprintf(stderr, "[Audio] crack.wav encontrado.\n");
        }
    }
#endif

    // ---- Pistas de creación de ventana -------------------------------------
    //     Borde/descoración deshabilitados: solo se verá el látigo.
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    //     Framebuffer transparente: el fondo queda 100% transparente.
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
    //     Antialiasing para que las líneas luzcan suaves.
    glfwWindowHint(GLFW_SAMPLES, 4);

    // -------------------------------------------------------------------------
    //  NOTA PARA WAYLAND / HYPRLAND:
    //  GLFW elige Wayland automáticamente si XDG_SESSION_TYPE=wayland. Para
    //  forzarlo:  export GLFW_PLATFORM=wayland  (requiere GLFW compilado con
    //  soporte Wayland). NO se usa ninguna API exclusiva de X11.
    //
    //  Para que Hyprland trate la ventana como flotante (sin mosaico) y
    //  "siempre visible", añade a ~/.config/hypr/hyprland.conf algo como:
    //
    //      windowrulev2 = float, class:^(whip_simulator)$
    //      windowrulev2 = stayfocused, class:^(whip_simulator)$
    //
    //  (El identificador 'class' se corresponde con el nombre del binario).
    // -------------------------------------------------------------------------

    // ---- Crear la ventana ----------------------------------------------------
    GLFWwindow* window = glfwCreateWindow(WINDOW_WIDTH, WINDOW_HEIGHT,
                                          "Látigo", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "Error: no se pudo crear la ventana GLFW.\n");
        glfwTerminate();
        return -1;
    }

    // ---- Ajustes específicos de Windows --------------------------------------
#ifdef _WIN32
    {
        // Obtener el manejador (HWND) nativo de la ventana de GLFW.
        HWND hwnd = glfwGetWin32Window(window);
        if (hwnd) {
            // Inyectar estilos extendidos nativos:
            //  WS_EX_LAYERED : habilita composición en capas (ventana "layered").
            //                  GLFW ya aplica alpha por píxel vía DWM; este
            //                  estilo queda disponible para composición manual.
            //  WS_EX_TOPMOST : mantiene la ventana siempre por encima de todas.
            LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
            exStyle |= WS_EX_LAYERED | WS_EX_TOPMOST;
            SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle);

            // Opcional (fallback de alpha uniforme, NO per-píxel):
            //   SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
            // Se omite para conservar la transparencia per-píxel de DWM.
        }
    }
#else
    // Linux/Wayland: no se necesita (ni se debe usar) código X11. La
    // transparencia y el posicionamiento se gestionan con el compositor
    // (Hyprland) mediante las reglas de ventana descritas más arriba.
#endif

    // ---- Configurar callbacks y estado inicial -------------------------------
    glfwSetMouseButtonCallback(window, mouseButtonCallback);
    glfwSetWindowUserPointer(window, nullptr); // dragging = false al inicio.

    // ---- Hacer actual el contexto OpenGL -------------------------------------
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // VSync: fluidez estable.

    // ---- Configuración de OpenGL (estado) ------------------------------------
    //     Color de limpieza totalmente transparente (Alpha = 0).
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_LINE_SMOOTH);
    glEnable(GL_MULTISAMPLE);
    glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);

    //     Proyección ortográfica en coordenadas de píxel (origen arriba-izq.).
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, WINDOW_WIDTH, WINDOW_HEIGHT, 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    // ---- Estado de la simulación ---------------------------------------------
    std::vector<Vec2> pos, prev;
    initWhip(pos, prev);

    bool  armed          = false;   // Listo para detectar un nuevo chasquido.
    float crackCooldown  = 0.0f;    // Temporizador entre chasquidos.

    double lastTime = glfwGetTime();

    // ---- Bucle principal -------------------------------------------------------
    while (!glfwWindowShouldClose(window)) {
        // --- Tiempo del frame (limitado para evitar saltos bruscos) ---
        double now = glfwGetTime();
        float dt = static_cast<float>(now - lastTime);
        lastTime = now;
        if (dt > 0.05f) dt = 0.05f;

        // --- Estado del ratón ---
        bool dragging = (glfwGetWindowUserPointer(window) != nullptr);
        Vec2 mouse = getMousePosition(window);

        // --- Física ---
        updatePhysics(pos, prev, mouse, dragging, dt);
        detectCrack(pos, prev, dt, crackCooldown, armed);

        // --- Render ---
        glViewport(0, 0, WINDOW_WIDTH, WINDOW_HEIGHT);
        glClear(GL_COLOR_BUFFER_BIT);

        renderWhip(pos);

        // Calcular el punto de agarre (grip) del mango.
        // El mouse sostiene el extremo trasero; la punta es pos[0].
        Vec2 grip;
        if (dragging) {
            grip = mouse;
        } else {
            grip.x = pos[0].x - HANDLE_LEN * std::cos(HANDLE_ANGLE);
            grip.y = pos[0].y - HANDLE_LEN * std::sin(HANDLE_ANGLE);
        }
        renderHandle(grip);

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    // ---- Limpieza ---------------------------------------------------------------
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
