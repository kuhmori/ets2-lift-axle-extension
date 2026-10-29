// in-game overlay: Dear ImGui drawn into the game's own DirectX 11 frame (Present hook via MinHook)
#include "common.h"
#include <type_traits>
#include <d3d11.h>
#include <dxgi1_2.h>
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "MinHook.h"

#pragma comment(lib, "d3d11.lib")

// ---------------------------------------------------------------------------
// state shared between the game thread (publishes) and the render thread (draws)
// ---------------------------------------------------------------------------
struct UiAxle { float pos = 0; bool up = false, controlled = false, releasing = false; };
struct UiSteer { float angle = 0; bool locking = false, releasing = false, lifted = false; };
struct UiVehicle {
    bool present = false, found = false, scanning = false, uncertain = false, canSwap = false, notRecognised = false, probing = false;
    char name[24] = "";
    unsigned axleCount = 0, allCount = 0, wheelObjects = 0;
    UiAxle axles[MAX_LIFT_AXLES];
    unsigned steerCount = 0;
    bool steerFound = false, steerSearching = false;
    UiSteer steer[MAX_STEER_AXLES];
    uintptr_t list = 0;
    int allIndex[MAX_LIFT_AXLES] = {};
};
struct UiState {
    Settings settings;
    UiVehicle v[VEHICLE_COUNT];
    bool blocked = false; char blockedReason[128] = "";
    char notice[200] = ""; double noticeAge = 1e9;
    char version[32] = ""; bool versionTested = false;
    int capture = -1;
};

static std::mutex g_uiMutex;
static UiState g_ui;
static_assert(std::is_trivially_destructible<UiState>::value, "UiState is wiped with memset");

// temporary copies hold addresses: clear them before the memory is freed (diagnostics search)
struct UiCopy {
    std::unique_ptr<UiState> p = std::make_unique<UiState>();
    ~UiCopy() { memset((void*)p.get(), 0, sizeof(UiState)); }
};
static std::mutex g_cmdMutex;
static std::vector<Command> g_cmds;

void PushCommand(const Command& c)
{
    std::lock_guard<std::mutex> lock(g_cmdMutex);
    g_cmds.push_back(c);
}

void OverlayPublish()
{
    if (!g_overlayVisible) return;
    UiCopy copy;
    UiState* st = copy.p.get();
    st->settings = g_settings;
    st->blocked = g_blocked;
    snprintf(st->blockedReason, sizeof(st->blockedReason), "%s", g_blockedReason);
    snprintf(st->notice, sizeof(st->notice), "%s", g_notice);
    st->noticeAge = NowSeconds() - g_noticeAt;
    snprintf(st->version, sizeof(st->version), "%s", g_gameVersion);
    st->versionTested = g_versionTested;
    st->capture = BindCaptureTarget();
    for (unsigned s = 0; s < VEHICLE_COUNT; ++s) {
        const Vehicle& v = g_vehicles[s];
        UiVehicle& u = st->v[s];
        u.present = v.present && (!v.axles.empty() || !v.steerAxles.empty());
        if (!u.present) continue;
        snprintf(u.name, sizeof(u.name), "%s", v.name);
        u.found = v.found;
        u.scanning = !v.found && (v.scanArmed || v.scanDone);
        u.notRecognised = !v.found && v.detectFails >= 2;
        u.uncertain = v.uncertain;
        u.probing = v.found && v.probeNeeded && ProbeActive();
        u.canSwap = CanSwapWithNext(s);
        u.axleCount = (unsigned)v.axles.size();
        u.allCount = (unsigned)std::min<size_t>(v.axleZ.size(), 16);
        u.wheelObjects = (unsigned)v.wheelTargets.size();
        u.list = v.axleList;
        for (unsigned i = 0; i < u.axleCount; ++i) {
            const Axle& a = v.axles[i];
            u.axles[i].pos = AxleUiPos(v, a);
            u.axles[i].up = AxleUiUp(v, a);
            u.axles[i].controlled = a.controlled;
            u.axles[i].releasing = a.releasing;
            u.allIndex[i] = (int)a.allIndex;
        }
        u.steerCount = (unsigned)v.steerAxles.size();
        u.steerFound = v.steerFound;
        u.steerSearching = v.steerScanDone && !v.steerFound;
        for (unsigned i = 0; i < u.steerCount; ++i) {
            const SteerAxle& a = v.steerAxles[i];
            float ang = 0.0f;
            for (unsigned w : a.wheels) if (std::fabs(v.wheels[w].steering) > std::fabs(ang)) ang = v.wheels[w].steering;
            u.steer[i].angle = ang * 360.0f;
            u.steer[i].locking = a.locking;
            u.steer[i].releasing = a.locking && !a.holding;
            u.steer[i].lifted = SteerAxleLifted(v, a);
        }
    }
    std::lock_guard<std::mutex> lock(g_uiMutex);
    g_ui = *st;
}

// game thread: carry out what the player clicked
void RunCommands()
{
    std::vector<Command> cmds;
    {
        std::lock_guard<std::mutex> lock(g_cmdMutex);
        cmds.swap(g_cmds);
    }
    for (const Command& c : cmds) {
        switch (c.type) {
        case CmdType::ToggleAxle: ToggleAxle(c.slot, c.axle); break;
        case CmdType::ReleaseVehicle: ReleaseVehicle(c.slot); break;
        case CmdType::ReleaseAll: ReleaseAll(); break;
        case CmdType::Rescan:
            if (c.slot >= VEHICLE_COUNT) break;
            RescanVehicle(c.slot); ResetSteer(g_vehicles[c.slot]);
            Notify("%s: lift or lower the axles once with the game key (and drive through a turn for the steering lock)", g_vehicles[c.slot].name);
            break;
        case CmdType::SetAuto:
            if (c.slot < VEHICLE_COUNT && c.axle < MAX_LIFT_AXLES) { g_settings.axleAuto[c.slot][c.axle] = c.value != 0; SaveSettings(); }
            break;
        case CmdType::SetSteerLock:   // value: bit 0 = lock, bit 1 = lock when reversing
            if (c.slot < VEHICLE_COUNT && c.axle < MAX_STEER_AXLES) {
                g_settings.steerLock[c.slot][c.axle] = (c.value & 1) != 0;
                g_settings.steerLockRev[c.slot][c.axle] = (c.value & 2) != 0;
                SaveSettings();
            }
            break;
        case CmdType::BindCapture: BeginBindCapture(c.value); break;
        case CmdType::ClearBind: ClearBind(c.value); break;
        case CmdType::ApplySettings: {
            Settings& s = g_settings; const Settings& n = c.settings;
            s.liftSeconds = n.liftSeconds; s.overlayMouseSpeed = n.overlayMouseSpeed; s.centerSeconds = n.centerSeconds;
            s.autoReverse = n.autoReverse; s.verboseLog = n.verboseLog; s.showDebug = n.showDebug;
            SaveSettings();
            break;
        }
        case CmdType::Diagnostics: WriteDiagnostics(); break;
        case CmdType::SwapTrailers: SwapWithNext(c.slot); break;
        case CmdType::CloseOverlay: SetOverlayVisible(false); break;
        }
    }
}

// ---------------------------------------------------------------------------
// mouse for the overlay
// ---------------------------------------------------------------------------
struct WinMouseMsg { UINT msg; WPARAM w; LPARAM l; };
static std::mutex g_msgMutex;
static std::vector<WinMouseMsg> g_msgs;

bool OverlayWindowMessage(HWND, UINT m, WPARAM w, LPARAM l)
{
    std::lock_guard<std::mutex> lock(g_msgMutex);
    if (g_msgs.size() < 512) g_msgs.push_back({ m, w, l });
    return true;
}

static ImVec2 g_cursor(-1, -1);
static unsigned g_lastButtons = 0;
static bool g_wasVisible = false;

static void FeedMouse(ImGuiIO& io, float speed)
{
    std::vector<WinMouseMsg> msgs;
    {
        std::lock_guard<std::mutex> lock(g_msgMutex);
        msgs.swap(g_msgs);
    }
    if (g_cursor.x < 0) g_cursor = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);

    float dx, dy, wheel; unsigned buttons;
    if (TakeVirtualMouse(dx, dy, wheel, buttons)) {
        // the game reads the mouse through DirectInput: use exactly that data (works in exclusive mode too)
        g_cursor.x = std::min(std::max(g_cursor.x + dx * speed, 0.0f), io.DisplaySize.x - 1);
        g_cursor.y = std::min(std::max(g_cursor.y + dy * speed, 0.0f), io.DisplaySize.y - 1);
        io.AddMousePosEvent(g_cursor.x, g_cursor.y);
        for (int b = 0; b < 3; ++b) {
            bool now = (buttons >> b) & 1, before = (g_lastButtons >> b) & 1;
            if (now != before) io.AddMouseButtonEvent(b, now);
        }
        g_lastButtons = buttons;
        if (wheel != 0) io.AddMouseWheelEvent(0, wheel);
        return;
    }
    // fallback: normal window messages
    for (auto& m : msgs) {
        float x = (float)(short)LOWORD(m.l), y = (float)(short)HIWORD(m.l);
        switch (m.msg) {
        case WM_MOUSEMOVE: g_cursor = ImVec2(x, y); io.AddMousePosEvent(x, y); break;
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: io.AddMouseButtonEvent(0, true); break;
        case WM_LBUTTONUP: io.AddMouseButtonEvent(0, false); break;
        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: io.AddMouseButtonEvent(1, true); break;
        case WM_RBUTTONUP: io.AddMouseButtonEvent(1, false); break;
        case WM_MOUSEWHEEL: io.AddMouseWheelEvent(0, (float)GET_WHEEL_DELTA_WPARAM(m.w) / (float)WHEEL_DELTA); break;
        }
    }
}

// ---------------------------------------------------------------------------
// the window
// ---------------------------------------------------------------------------
static void Cmd(CmdType t, unsigned slot = 0, unsigned axle = 0, int value = 0)
{
    Command c; c.type = t; c.slot = slot; c.axle = axle; c.value = value;
    PushCommand(c);
}

// a button that shows a keybind; click = change, right click = remove
static void BindButton(const char* text, int target, int capture, float width)
{
    ImGui::PushID(target);
    bool waiting = capture == target;
    if (waiting) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.50f, 0.10f, 1.0f));
    if (ImGui::Button(waiting ? "press..." : text, ImVec2(width, 0))) Cmd(CmdType::BindCapture, 0, 0, target);
    if (waiting) ImGui::PopStyleColor();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) Cmd(CmdType::ClearBind, 0, 0, target);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(target == BIND_OVERLAY ? "Click, then press the new key. Esc cancels." :
                          "Click, then press the new key/button.\nEsc cancels, Backspace or right click removes it.");
    ImGui::PopID();
}

static void AxleState(const UiAxle& a, char* out, size_t n)
{
    const char* s = a.releasing ? "returning" : (a.pos > 0.01f && a.pos < 0.99f) ? (a.up ? "lifting" : "lowering") : (a.up ? "UP" : "DOWN");
    snprintf(out, n, "%s%s", s, a.controlled && !a.releasing ? " *" : "");
}

static void DrawVehicle(const UiState& st, unsigned slot)
{
    const UiVehicle& u = st.v[slot];
    const Settings& s = st.settings;
    ImGui::PushID((int)slot);
    if (!ImGui::CollapsingHeader(u.name, ImGuiTreeNodeFlags_DefaultOpen)) { ImGui::PopID(); return; }

    if (u.axleCount && !u.found) {
        if (u.notRecognised)
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Lift data not recognised (game %s). This game version may not be supported yet.", st.version);
        else
            ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Setup: lift or lower these axles once with the game's own lift key.");
    }
    if (u.probing) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Checking which trailer is which...");
    else if (u.canSwap) {
        ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Identical trailers - if the wrong one moves:");
        ImGui::SameLine();
        if (ImGui::SmallButton("Swap with next trailer")) Cmd(CmdType::SwapTrailers, slot);
    }

    if (u.axleCount && ImGui::BeginTable("axles", 5, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Axle", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn("Position", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn("Auto", ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("Controller", ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableHeadersRow();
        for (unsigned i = 0; i < u.axleCount; ++i) {
            const UiAxle& a = u.axles[i];
            ImGui::PushID((int)i);
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            char label[48];
            snprintf(label, sizeof(label), "Lift axle %u%s", i + 1, u.axleCount > 1 && i == 0 ? " (front)" : u.axleCount > 1 && i == u.axleCount - 1 ? " (rear)" : "");
            if (a.up) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.50f, 0.30f, 1.0f));
            ImGui::BeginDisabled(!u.found || st.blocked);
            if (ImGui::Button(label, ImVec2(-FLT_MIN, 0))) Cmd(CmdType::ToggleAxle, slot, i);
            ImGui::EndDisabled();
            if (a.up) ImGui::PopStyleColor();

            ImGui::TableNextColumn();
            char state[32]; AxleState(a, state, sizeof(state));
            ImGui::ProgressBar(std::min(std::max(a.pos, 0.0f), 1.0f), ImVec2(-FLT_MIN, 0), state);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("* = controlled by this plugin");

            ImGui::TableNextColumn();
            bool autoOn = s.axleAuto[slot][i];
            if (ImGui::Checkbox("##auto", &autoOn)) Cmd(CmdType::SetAuto, slot, i, autoOn ? 1 : 0);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Automatic mode for this axle: lift or lower it when reversing (set in Settings)");

            ImGui::TableNextColumn();
            char t[64]; KeyBindText(s.axle[slot][i].key, t, sizeof(t));
            BindButton(t, BindAxleKey(slot, i), st.capture, -FLT_MIN);
            ImGui::TableNextColumn();
            JoyBindText(s.axle[slot][i].joy, t, sizeof(t));
            BindButton(t, BindAxleJoy(slot, i), st.capture, -FLT_MIN);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (u.axleCount) {
        ImGui::BeginDisabled(!u.found || st.blocked);
        if (ImGui::Button("Release to game")) Cmd(CmdType::ReleaseVehicle, slot);
        ImGui::EndDisabled();
    }

    // steered axles: lock straight while reversing
    if (u.steerCount) {
        if (!u.steerFound)
            ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "%s", u.steerSearching ? "Steering lock setup: keep driving through a few turns..."
                                                                                : "Steering lock setup: drive through a turn once (the steered axles have to move).");
        if (ImGui::BeginTable("steer", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Steered axle", ImGuiTableColumnFlags_WidthFixed, 120);
            ImGui::TableSetupColumn("Steering", ImGuiTableColumnFlags_WidthFixed, 120);
            ImGui::TableSetupColumn("Lock", ImGuiTableColumnFlags_WidthFixed, 40);
            ImGui::TableSetupColumn("Reverse", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableHeadersRow();
            for (unsigned i = 0; i < u.steerCount; ++i) {
                const UiSteer& a = u.steer[i];
                ImGui::PushID(100 + (int)i);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("Steered axle %u%s", i + 1, u.steerCount > 1 && i == 0 ? " (front)" : u.steerCount > 1 && i == u.steerCount - 1 ? " (rear)" : "");
                ImGui::TableNextColumn();
                if (a.releasing) ImGui::Text("releasing %+.1f deg", a.angle);
                else if (a.locking) ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1), std::fabs(a.angle) > 0.3f ? "centering %+.1f deg" : "LOCKED %+.1f deg", a.angle);
                else if (a.lifted) ImGui::TextDisabled("lifted - held straight");
                else ImGui::Text("free %+.1f deg", a.angle);
                ImGui::TableNextColumn();
                bool lock = s.steerLock[slot][i], rev = s.steerLockRev[slot][i];
                if (ImGui::Checkbox("##lock", &lock)) Cmd(CmdType::SetSteerLock, slot, i, (lock ? 1 : 0) | (rev ? 2 : 0));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lock this axle straight all the time.");
                ImGui::TableNextColumn();
                ImGui::BeginDisabled(lock);
                if (ImGui::Checkbox("##rev", &rev)) Cmd(CmdType::SetSteerLock, slot, i, (lock ? 1 : 0) | (rev ? 2 : 0));
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Lock this axle only while reversing: the wheels are centered smoothly\nand steer freely again when you drive forward.");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    ImGui::Spacing();
    ImGui::PopID();
}

static void DrawSettings(const UiState& st)
{
    if (!ImGui::CollapsingHeader("Settings")) return;
    Settings s = st.settings;
    bool changed = false;
    char t[64];

    ImGui::SeparatorText("General");
    changed |= ImGui::SliderFloat("Lift / lower time", &s.liftSeconds, 0.5f, 10.0f, "%.1f s");
    KeyBindText(s.overlayKey, t, sizeof(t));
    ImGui::Text("Overlay key"); ImGui::SameLine(160); BindButton(t, BIND_OVERLAY, st.capture, 120);
    changed |= ImGui::SliderFloat("Overlay mouse speed", &s.overlayMouseSpeed, 0.25f, 4.0f, "%.2f");
    changed |= ImGui::SliderFloat("Steering centering time", &s.centerSeconds, 0.2f, 5.0f, "%.1f s");

    ImGui::SeparatorText("Automatic mode (axles with 'Auto' ticked)");
    int rev = (int)s.autoReverse;
    if (ImGui::Combo("When reversing", &rev, "No change\0Lower\0Lift\0")) { s.autoReverse = (ReverseRule)rev; changed = true; }
    ImGui::TextDisabled("The axles go back to their previous state when you drive forward again.");

    ImGui::SeparatorText("Advanced");
    changed |= ImGui::Checkbox("Verbose log (game.log.txt)", &s.verboseLog);
    changed |= ImGui::Checkbox("Show debug tools", &s.showDebug);

    if (changed) {
        Command c; c.type = CmdType::ApplySettings; c.settings = s;
        PushCommand(c);
    }
}

static void DrawDebug(const UiState& st)
{
    if (!st.settings.showDebug || !ImGui::CollapsingHeader("Debug")) return;
    ImGui::Text("Game %s (%s), plugin %s", st.version, st.versionTested ? "tested" : "untested", AXLELIFT_VERSION);
    for (unsigned s = 0; s < VEHICLE_COUNT; ++s) {
        const UiVehicle& u = st.v[s];
        if (!u.present) continue;
        ImGui::PushID((int)s);
        ImGui::Text("%s: %s, lift data %p, wheel objects %u, steering %s", u.name, u.found ? "found" : u.scanning ? "searching" : "-",
                    (void*)u.list, u.wheelObjects, u.steerFound ? "found" : u.steerSearching ? "searching" : "-");
        ImGui::SameLine();
        if (ImGui::SmallButton("Search again")) Cmd(CmdType::Rescan, s);
        ImGui::PopID();
    }
    if (ImGui::Button("Write diagnostics file")) Cmd(CmdType::Diagnostics);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Writes axlelift_diagnostics.txt next to the plugin (for the developer).\nThe game freezes for about 10-20 seconds while it searches.");
}

static void DrawUi()
{
    UiCopy copy;
    {
        std::lock_guard<std::mutex> lock(g_uiMutex);
        *copy.p = g_ui;
    }
    const UiState& st = *copy.p;
    const Settings& s = st.settings;

    ImGui::SetNextWindowSize(ImVec2(640, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(60, 80), ImGuiCond_FirstUseEver);
    bool open = true;
    if (!ImGui::Begin("Lift Axle Extension", &open, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
    if (!open) Cmd(CmdType::CloseOverlay);

    char t[64]; KeyBindText(s.overlayKey, t, sizeof(t));
    ImGui::TextDisabled("%s opens / closes this window. Click an axle to lift or lower it.", t);
    if (st.blocked) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Disabled: %s", st.blockedReason);
    if (!st.versionTested && st.version[0])
        ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Game version %s has not been tested with this plugin.", st.version);
    if (st.notice[0] && st.noticeAge < 8.0) ImGui::TextWrapped("%s", st.notice);
    if (st.capture >= 0) ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "Press the new key or controller button. Esc cancels, Backspace removes.");
    ImGui::Separator();

    bool any = false;
    for (unsigned v = 0; v < VEHICLE_COUNT; ++v) if (st.v[v].present) { DrawVehicle(st, v); any = true; }
    if (!any) ImGui::TextDisabled("No truck or trailer with lift or steered axles right now.");

    ImGui::Separator();
    ImGui::BeginDisabled(st.blocked);
    if (ImGui::Button("Release all to game")) Cmd(CmdType::ReleaseAll);
    ImGui::EndDisabled();
    ImGui::SameLine(); ImGui::TextDisabled("Key"); ImGui::SameLine();
    KeyBindText(s.releaseAll.key, t, sizeof(t)); BindButton(t, BIND_RELEASE_KEY, st.capture, 110);
    ImGui::SameLine(); ImGui::TextDisabled("Controller"); ImGui::SameLine();
    JoyBindText(s.releaseAll.joy, t, sizeof(t)); BindButton(t, BIND_RELEASE_JOY, st.capture, 150);

    DrawSettings(st);
    DrawDebug(st);
    ImGui::End();
}

// ---------------------------------------------------------------------------
// DirectX 11 hooks
// ---------------------------------------------------------------------------
typedef HRESULT (STDMETHODCALLTYPE* PresentFn)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE* Present1Fn)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT (STDMETHODCALLTYPE* ResizeFn)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

static PresentFn  g_origPresent = nullptr;
static Present1Fn g_origPresent1 = nullptr;
static ResizeFn   g_origResize = nullptr;
static void* g_addrPresent = nullptr; static void* g_addrPresent1 = nullptr; static void* g_addrResize = nullptr;

static std::atomic<int>  g_inHook{0};
static std::atomic<bool> g_stopping{false};
static IDXGISwapChain* g_swap = nullptr;          // the game's swap chain (not AddRef'd)
static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static bool g_imguiReady = false, g_unsupported = false;
static LARGE_INTEGER g_qpf{}, g_lastFrame{};
static thread_local bool t_rendering = false;

static bool InitImGui(IDXGISwapChain* sc)
{
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&g_dev)) || !g_dev) { g_unsupported = true; return false; }
    g_dev->GetImmediateContext(&g_ctx);
    DXGI_SWAP_CHAIN_DESC d{};
    sc->GetDesc(&d);
    HookGameWindow(d.OutputWindow);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;          // window layout is not saved to disk
    io.LogFilename = nullptr;
    io.MouseDrawCursor = true;         // the game hides the Windows cursor
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 4.0f;
    if (!ImGui_ImplDX11_Init(g_dev, g_ctx)) { ImGui::DestroyContext(); g_unsupported = true; return false; }
    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_lastFrame);
    g_swap = sc;
    g_imguiReady = true;
    return true;
}

static void RenderOverlay(IDXGISwapChain* sc)
{
    if (g_stopping || g_unsupported || t_rendering) return;
    t_rendering = true;
    if (!g_imguiReady && !InitImGui(sc)) { t_rendering = false; return; }
    if (sc != g_swap) { t_rendering = false; return; }   // some other swap chain (e.g. a launcher window)

    bool visible = g_overlayVisible;
    if (!visible) {
        if (g_wasVisible) { g_cursor = ImVec2(-1, -1); g_lastButtons = 0; }
        g_wasVisible = false;
        t_rendering = false;
        return;
    }
    g_wasVisible = true;

    if (!g_rtv) {
        ID3D11Texture2D* back = nullptr;
        if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) && back) {
            g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
            back->Release();
        }
        if (!g_rtv) { t_rendering = false; return; }
    }

    DXGI_SWAP_CHAIN_DESC d{};
    sc->GetDesc(&d);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)d.BufferDesc.Width, (float)d.BufferDesc.Height);
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    io.DeltaTime = std::max((float)(now.QuadPart - g_lastFrame.QuadPart) / (float)g_qpf.QuadPart, 1.0f / 1000.0f);
    g_lastFrame = now;

    float speed;
    {
        std::lock_guard<std::mutex> lock(g_uiMutex);
        speed = g_ui.settings.overlayMouseSpeed;
    }
    FeedMouse(io, speed);

    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    DrawUi();
    ImGui::Render();

    ID3D11RenderTargetView* oldRtv = nullptr; ID3D11DepthStencilView* oldDsv = nullptr;
    g_ctx->OMGetRenderTargets(1, &oldRtv, &oldDsv);
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_ctx->OMSetRenderTargets(1, &oldRtv, oldDsv);
    if (oldRtv) oldRtv->Release();
    if (oldDsv) oldDsv->Release();
    t_rendering = false;
}

static HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    ++g_inHook;
    if (!(flags & DXGI_PRESENT_TEST)) RenderOverlay(sc);
    HRESULT hr = g_origPresent(sc, sync, flags);
    --g_inHook;
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p)
{
    ++g_inHook;
    if (!(flags & DXGI_PRESENT_TEST)) RenderOverlay(sc);
    HRESULT hr = g_origPresent1(sc, sync, flags, p);
    --g_inHook;
    return hr;
}

static HRESULT STDMETHODCALLTYPE HookResize(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl)
{
    ++g_inHook;
    if (sc == g_swap && g_rtv) { g_rtv->Release(); g_rtv = nullptr; }  // recreated on the next frame
    HRESULT hr = g_origResize(sc, n, w, h, f, fl);
    --g_inHook;
    return hr;
}

// find the DXGI functions through a throwaway device + swap chain on a hidden window
static bool FindSwapChainFunctions()
{
    WNDCLASSEXA wc{ sizeof(wc) };
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "AxleLiftDxProbe";
    RegisterClassExA(&wc);
    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return false;

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.Width = 64; sd.BufferDesc.Height = 64;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* swap = nullptr; ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                               &sd, &swap, &dev, nullptr, &ctx);
    if (FAILED(hr))
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                           &sd, &swap, &dev, nullptr, &ctx);
    if (SUCCEEDED(hr) && swap) {
        void** vt = *(void***)swap;
        g_addrPresent = vt[8];
        g_addrResize = vt[13];
        IDXGISwapChain1* swap1 = nullptr;
        if (SUCCEEDED(swap->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&swap1)) && swap1) {
            g_addrPresent1 = (*(void***)swap1)[22];
            swap1->Release();
        }
    }
    if (swap) swap->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    DestroyWindow(hwnd);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
    return g_addrPresent != nullptr;
}

void OverlayInit()
{
    if (!FindSwapChainFunctions()) { Log(SCS_LOG_TYPE_error, "[axlelift] overlay: DirectX 11 not available"); return; }
    if (MH_Initialize() != MH_OK) { Log(SCS_LOG_TYPE_error, "[axlelift] overlay: MinHook failed"); return; }
    bool ok = MH_CreateHook(g_addrPresent, (void*)&HookPresent, (void**)&g_origPresent) == MH_OK &&
              MH_CreateHook(g_addrResize, (void*)&HookResize, (void**)&g_origResize) == MH_OK;
    if (g_addrPresent1) MH_CreateHook(g_addrPresent1, (void*)&HookPresent1, (void**)&g_origPresent1);
    if (!ok || MH_EnableHook(MH_ALL_HOOKS) != MH_OK) { Log(SCS_LOG_TYPE_error, "[axlelift] overlay: could not hook DirectX"); return; }
    LogDebug("[axlelift] overlay hooks installed");
}

void OverlayShutdown()
{
    g_stopping = true;
    MH_DisableHook(MH_ALL_HOOKS);
    for (int i = 0; i < 200 && g_inHook > 0; ++i) Sleep(5);    // let a frame in progress finish
    MH_Uninitialize();
    if (g_imguiReady) {
        ImGui_ImplDX11_Shutdown();
        ImGui::DestroyContext();
        g_imguiReady = false;
    }
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
}
