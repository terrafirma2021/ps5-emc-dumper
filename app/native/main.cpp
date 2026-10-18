#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include "imgui.h"
#include "imgui_impl_opengl3.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

struct alignas(8) PadState {
    std::uint32_t buttons;
    unsigned char unused0[72];
    std::int32_t connected;
    unsigned char unused1[40];
};
static_assert(sizeof(PadState) == 120, "pad ABI");
static_assert(offsetof(PadState, connected) == 0x4c, "pad ABI");

extern "C" {
int isatty(int) { return 0; }
int mkstemp(char*) { return -1; }
int sceUserServiceInitialize(void*);
int sceUserServiceGetInitialUser(int*);
int sceUserServiceTerminate();
int scePadInit();
int scePadOpen(int, int, int, const void*);
int scePadReadState(int, PadState*);
int scePadClose(int);
int sceSystemServiceLoadExec(const char*, const char**);
int sceNetSocket(const char*, int, int, int);
int sceNetSocketClose(int);
int sceNetConnect(int, const void*, std::uint32_t);
int sceNetSend(int, const void*, std::size_t, int);
int sceNetSetsockopt(int, int, int, const void*, std::uint32_t);
}

namespace {
constexpr std::uint32_t kCross = 0x4000;
constexpr std::uint32_t kCircle = 0x2000;
constexpr const char* kStatusPath = "/app0/emc-status.json";
constexpr const char* kResultPath = "/app0/emc-result.json";
constexpr const char* kDisplayPath = "/app0/emc-display.txt";

struct NetAddress {
    std::uint8_t length, family;
    std::uint16_t port;
    std::uint32_t address;
    std::uint16_t virtual_port;
    std::uint8_t zero[6];
};

enum class ReaderState { Idle, Starting, Reading, Complete, SaveFailed, Failed };

bool send_all(int socket, const char* data, std::size_t size)
{
    while (size) {
        const int sent = sceNetSend(socket, data, size, 0);
        if (sent <= 0) return false;
        data += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

bool launch_reader(char* error, std::size_t error_size)
{
    FILE* input = std::fopen("/app0/emc-reader.elf", "rb");
    if (!input) {
        std::snprintf(error, error_size, "Missing /app0/emc-reader.elf");
        return false;
    }

    const int socket = sceNetSocket("emc-native-reader", 2, 1, 6);
    const int timeout_us = 5000000;
    const NetAddress address{sizeof(NetAddress), 2,
        static_cast<std::uint16_t>((9021u << 8) | (9021u >> 8)),
        0x0100007f, 0, {}};
    bool ok = socket >= 0 &&
        sceNetSetsockopt(socket, 0xffff, 0x1105, &timeout_us, sizeof timeout_us) >= 0 &&
        sceNetSetsockopt(socket, 0xffff, 0x1109, &timeout_us, sizeof timeout_us) >= 0 &&
        sceNetConnect(socket, &address, sizeof address) >= 0;
    char buffer[65536];
    while (ok) {
        const std::size_t count = std::fread(buffer, 1, sizeof buffer, input);
        if (count == 0) { ok = std::feof(input); break; }
        ok = send_all(socket, buffer, count);
    }
    std::fclose(input);
    if (socket >= 0) sceNetSocketClose(socket);
    if (!ok)
        std::snprintf(error, error_size,
            "ELF loader on port 9021 unavailable; load it and relaunch");
    return ok;
}

bool read_text(const char* path, char* output, std::size_t size)
{
    FILE* input = std::fopen(path, "rb");
    if (!input) return false;
    const std::size_t count = std::fread(output, 1, size - 1, input);
    std::fclose(input);
    if (!count) return false;
    output[count] = '\0';
    return true;
}

unsigned json_unsigned(const char* json, const char* key)
{
    const char* at = std::strstr(json, key);
    unsigned value = 0;
    if (at) std::sscanf(at + std::strlen(key), "%*[^0-9]%u", &value);
    return value;
}

ReaderState update_state(ReaderState current, unsigned& code,
                         unsigned& bytes, unsigned& populated)
{
    char text[1024];
    if (!read_text(kStatusPath, text, sizeof text)) return current;
    code = json_unsigned(text, "\"code\"");
    if (std::strstr(text, "\"state\": \"reading\"")) return ReaderState::Reading;
    if (std::strstr(text, "\"state\": \"save-failed\"")) return ReaderState::SaveFailed;
    if (std::strstr(text, "\"state\": \"failed\"")) return ReaderState::Failed;
    if (!std::strstr(text, "\"state\": \"complete\"")) return current;
    if (read_text(kResultPath, text, sizeof text)) {
        bytes = json_unsigned(text, "\"bytes\"");
        populated = json_unsigned(text, "\"populatedSlots\"");
    }
    return ReaderState::Complete;
}

const char* heading(ReaderState state)
{
    switch (state) {
    case ReaderState::Idle: return "READY";
    case ReaderState::Starting: return "STARTING EMC READER";
    case ReaderState::Reading: return "READING PERSISTENT ERROR LOG";
    case ReaderState::Complete: return "EMC QUERY COMPLETE";
    case ReaderState::SaveFailed: return "QUERY COMPLETE - SAVE FAILED";
    case ReaderState::Failed: return "EMC QUERY FAILED";
    }
    return "EMC READER";
}

void draw_chip_mark(ImDrawList* draw, ImVec2 origin)
{
    const ImU32 green = IM_COL32(53, 224, 139, 255);
    const ImU32 blue = IM_COL32(112, 157, 218, 255);
    const ImU32 dark = IM_COL32(10, 20, 34, 255);
    draw->AddRectFilled(origin, ImVec2(origin.x + 82, origin.y + 68), dark, 12);
    draw->AddRect(origin, ImVec2(origin.x + 82, origin.y + 68), blue, 12, 0, 3);
    for (int i = 0; i < 4; ++i) {
        const float x = origin.x + 13 + i * 19;
        draw->AddRectFilled(ImVec2(x, origin.y - 8), ImVec2(x + 8, origin.y), green, 2);
        draw->AddRectFilled(ImVec2(x, origin.y + 68), ImVec2(x + 8, origin.y + 76), green, 2);
    }
    for (int i = 0; i < 3; ++i) {
        const float y = origin.y + 12 + i * 20;
        draw->AddRectFilled(ImVec2(origin.x - 8, y), ImVec2(origin.x, y + 8), green, 2);
        draw->AddRectFilled(ImVec2(origin.x + 82, y), ImVec2(origin.x + 90, y + 8), green, 2);
    }
    draw->AddText(ImGui::GetFont(), 25, ImVec2(origin.x + 13, origin.y + 20),
                  IM_COL32(239, 246, 255, 255), "EMC");
}

void draw_button_hint(ImDrawList* draw, ImVec2 at, ImU32 color,
                      const char* symbol, const char* label)
{
    draw->AddCircleFilled(ImVec2(at.x + 18, at.y + 18), 18, color);
    const ImVec2 symbol_size = ImGui::CalcTextSize(symbol);
    draw->AddText(ImVec2(at.x + 18 - symbol_size.x * .5f,
                         at.y + 18 - symbol_size.y * .5f),
                  IM_COL32(8, 14, 26, 255), symbol);
    draw->AddText(ImGui::GetFont(), 23, ImVec2(at.x + 50, at.y + 3),
                  IM_COL32(220, 230, 244, 255), label);
}

bool render_loop(EGLDisplay display, EGLSurface surface)
{
    EGLint width = 0, height = 0;
    if (!eglQuerySurface(display, surface, EGL_WIDTH, &width) ||
        !eglQuerySurface(display, surface, EGL_HEIGHT, &height)) return false;

    const bool owns_user = sceUserServiceInitialize(nullptr) == 0;
    int user = -1, pad = -1;
    if (sceUserServiceGetInitialUser(&user) == 0 && scePadInit() >= 0)
        pad = scePadOpen(user, 0, 0, nullptr);

    ReaderState reader_state = ReaderState::Idle;
    unsigned code = 0, bytes = 0, populated = 0;
    unsigned poll_delay = 0;
    bool attempted = false;
    char launch_error[160] = {};
    char decoded_summary[2048] = {};
    bool previous_cross = false;
    bool previous_circle = false;
    bool running = true;
    bool ok = true;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1920, 1080);
    io.DisplayFramebufferScale = ImVec2(width / 1920.0f, height / 1080.0f);

    while (running && ok) {
        PadState state{};
        const bool connected = pad >= 0 && scePadReadState(pad, &state) >= 0 &&
            state.connected && !(state.buttons & 0x80000000u);
        const bool circle = connected && (state.buttons & kCircle);
        const bool cross = connected && (state.buttons & kCross);
        if (circle && !previous_circle) {
            (void)sceSystemServiceLoadExec("exit", nullptr);
            running = false;
        }
        if (cross && !previous_cross && !attempted && reader_state == ReaderState::Idle) {
            attempted = true;
            reader_state = ReaderState::Starting;
            code = bytes = populated = 0;
            poll_delay = 60;
            launch_error[0] = '\0';
            if (!launch_reader(launch_error, sizeof launch_error)) {
                reader_state = ReaderState::Failed;
                code = 1;
            }
        }
        if (reader_state == ReaderState::Starting || reader_state == ReaderState::Reading) {
            if (poll_delay) --poll_delay;
            else reader_state = update_state(reader_state, code, bytes, populated);
        }
        if (reader_state == ReaderState::Complete && !decoded_summary[0])
            (void)read_text(kDisplayPath, decoded_summary, sizeof decoded_summary);
        previous_cross = cross;
        previous_circle = circle;

        io.DeltaTime = 1.0f / 60.0f;
        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();
        ImDrawList* draw = ImGui::GetBackgroundDrawList();
        const ImU32 background = IM_COL32(7, 13, 24, 255);
        const ImU32 panel = IM_COL32(16, 27, 44, 255);
        const ImU32 panel_light = IM_COL32(21, 35, 56, 255);
        const ImU32 border = IM_COL32(44, 63, 88, 255);
        const ImU32 muted = IM_COL32(139, 159, 187, 255);
        const ImU32 white = IM_COL32(238, 245, 255, 255);
        draw->AddRectFilled(ImVec2(0, 0), ImVec2(1920, 1080), background);

        // Header
        draw_chip_mark(draw, ImVec2(112, 81));
        draw->AddText(ImGui::GetFont(), 42, ImVec2(230, 78), white, "PS5 EMC DUMPER");
        draw->AddText(ImGui::GetFont(), 21, ImVec2(232, 133), muted,
                      "Persistent error-log reader  /  Firmware 13.60");
        draw->AddRectFilled(ImVec2(1558, 92), ImVec2(1798, 140), panel_light, 24);
        draw->AddCircleFilled(ImVec2(1590, 116), 6, IM_COL32(53, 224, 139, 255));
        draw->AddText(ImGui::GetFont(), 20, ImVec2(1610, 101), muted, "ONE-SHOT MODE");

        const bool failed = reader_state == ReaderState::Failed ||
                            reader_state == ReaderState::SaveFailed;
        const ImU32 accent = failed ? IM_COL32(235, 87, 87, 255) :
                                     IM_COL32(53, 224, 139, 255);

        // Primary status card
        draw->AddRectFilled(ImVec2(110, 205), ImVec2(1810, 590), panel, 20);
        draw->AddRect(ImVec2(110, 205), ImVec2(1810, 590), border, 20, 0, 2);
        draw->AddRectFilled(ImVec2(110, 205), ImVec2(120, 590), accent, 20,
                            ImDrawFlags_RoundCornersLeft);
        draw->AddText(ImGui::GetFont(), 20, ImVec2(165, 250), muted, "CURRENT STATUS");
        draw->AddCircleFilled(ImVec2(180, 332), 16, accent);
        draw->AddCircle(ImVec2(180, 332), 26, IM_COL32(53, 224, 139, 55), 0, 8);
        draw->AddText(ImGui::GetFont(), 42, ImVec2(225, 301), white, heading(reader_state));
        char detail[256];
        if (reader_state == ReaderState::Idle)
            std::snprintf(detail, sizeof detail, "Press X to read the persistent EMC error log.");
        else if (reader_state == ReaderState::Complete)
            std::snprintf(detail, sizeof detail, "%u RAW BYTES  /  %u OF 63 SLOTS POPULATED",
                          bytes, populated);
        else if (failed) {
            if (*launch_error) std::snprintf(detail, sizeof detail, "%s", launch_error);
            else std::snprintf(detail, sizeof detail,
                               "FAILURE CODE: %u  /  REBOOT BEFORE ANOTHER ATTEMPT", code);
        } else if (reader_state == ReaderState::Reading && code >= 100)
            std::snprintf(detail, sizeof detail, "Please wait. Diagnostic stage: %u", code);
        else
            std::snprintf(detail, sizeof detail, "Please wait while the one-shot query completes.");
        draw->AddText(ImGui::GetFont(), 25, ImVec2(225, 370), muted, detail);
        draw->AddRectFilled(ImVec2(165, 455), ImVec2(1755, 459), IM_COL32(33, 49, 72, 255), 2);
        float progress = reader_state == ReaderState::Idle ? 0.0f :
                         reader_state == ReaderState::Starting ? 0.22f :
                         reader_state == ReaderState::Reading ? 0.62f : 1.0f;
        if (progress > 0)
            draw->AddRectFilled(ImVec2(165, 455), ImVec2(165 + 1590 * progress, 459), accent, 2);
        const char* safety = attempted ? "Attempt used - reboot required before another read" :
                                         "No EMC access has occurred in this session";
        draw->AddText(ImGui::GetFont(), 20, ImVec2(165, 495), muted, safety);

        // Result and guidance cards
        draw->AddRectFilled(ImVec2(110, 620), ImVec2(1250, 885), panel, 20);
        draw->AddRect(ImVec2(110, 620), ImVec2(1250, 885), border, 20, 0, 2);
        draw->AddText(ImGui::GetFont(), 22, ImVec2(155, 657), muted,
                      reader_state == ReaderState::Complete ? "DECODED SUMMARY" : "OUTPUT");
        if (reader_state == ReaderState::Complete) {
            draw->AddText(ImGui::GetFont(), 20, ImVec2(155, 705), accent,
                          "Saved to /data/emc-error-log/");
            if (decoded_summary[0])
                draw->AddText(ImGui::GetFont(), 18, ImVec2(155, 746), white, decoded_summary);
        } else {
            draw->AddText(ImGui::GetFont(), 23, ImVec2(155, 713), white,
                          "Raw dump     latest.bin");
            draw->AddText(ImGui::GetFont(), 23, ImVec2(155, 759), white,
                          "Decoded      decoded.txt + latest.json");
            draw->AddText(ImGui::GetFont(), 19, ImVec2(155, 817), muted,
                          "Files are created only after a successful read.");
        }
        draw->AddRectFilled(ImVec2(1280, 620), ImVec2(1810, 885), panel_light, 20);
        draw->AddRect(ImVec2(1280, 620), ImVec2(1810, 885), border, 20, 0, 2);
        draw->AddText(ImGui::GetFont(), 22, ImVec2(1325, 657), muted, "DATA NOTICE");
        draw->AddText(ImGui::GetFont(), 23, ImVec2(1325, 710), white,
                      "Raw dump is authoritative.");
        draw->AddText(ImGui::GetFont(), 20, ImVec2(1325, 757), muted,
                      "Decoded labels are community-derived\nand intentionally conservative.");
        draw->AddText(ImGui::GetFont(), 19, ImVec2(1325, 831), IM_COL32(112, 157, 218, 255),
                      "63 records  /  2,020 bytes");

        draw->AddRectFilled(ImVec2(0, 930), ImVec2(1920, 1080), IM_COL32(10, 18, 31, 255));
        if (!attempted)
            draw_button_hint(draw, ImVec2(125, 976), IM_COL32(65, 155, 255, 255), "X", "READ EMC LOG");
        draw_button_hint(draw, ImVec2(attempted ? 125 : 385, 976),
                         IM_COL32(235, 87, 87, 255), "O", "EXIT TO HOME");
        draw->AddText(ImGui::GetFont(), 19, ImVec2(1430, 983), muted,
                      "PPSA99760  |  v0.1.0");
        ImGui::Render();
        glViewport(0, 0, width, height);
        glClearColor(8.0f / 255, 14.0f / 255, 26.0f / 255, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        ok = glGetError() == GL_NO_ERROR && eglSwapBuffers(display, surface);
    }

    if (pad >= 0) scePadClose(pad);
    if (owns_user) sceUserServiceTerminate();
    return ok;
}
}

int main()
{
    const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    const EGLint context_attributes[] = {
        EGL_CONTEXT_MAJOR_VERSION_KHR, 3, EGL_CONTEXT_MINOR_VERSION_KHR, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR, EGL_NONE};
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLConfig config = nullptr;
    EGLint count = 0;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    bool ok = display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr) &&
        eglBindAPI(EGL_OPENGL_API) &&
        eglChooseConfig(display, config_attributes, &config, 1, &count) && count == 1;
    if (ok) {
        surface = eglCreateWindowSurface(display, config, (EGLNativeWindowType)0, nullptr);
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
        ok = surface != EGL_NO_SURFACE && context != EGL_NO_CONTEXT &&
            eglMakeCurrent(display, surface, surface, context);
    }
    if (ok) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImFontConfig font;
        font.SizePixels = 30;
        ImGui::GetIO().Fonts->AddFontDefault(&font);
        ok = ImGui_ImplOpenGL3_Init("#version 330 core") && render_loop(display, surface);
        ImGui_ImplOpenGL3_Shutdown();
        ImGui::DestroyContext();
    }
    if (display != EGL_NO_DISPLAY) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
        if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
        eglTerminate(display);
    }
    return ok ? 0 : 1;
}
