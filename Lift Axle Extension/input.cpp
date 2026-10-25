// input: keyboard + wheel/gamepad buttons, hotkeys, keybind capture,
// and taking the mouse away from the game while the overlay is open
#include "common.h"
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")

std::atomic<bool> g_overlayVisible{false};
HWND g_gameHwnd = nullptr;

// ---------------------------------------------------------------------------
// keyboard (one snapshot per game frame)
// ---------------------------------------------------------------------------
static bool g_keyDown[256], g_keyPressed[256];

static void ReadKeyboard()
{
    for (int vk = 1; vk < 256; ++vk) {
        bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        g_keyPressed[vk] = down && !g_keyDown[vk];
        g_keyDown[vk] = down;
    }
}

static bool ModsDown(const KeyBind& b)
{
    return g_keyDown[VK_CONTROL] == b.ctrl && g_keyDown[VK_MENU] == b.alt && g_keyDown[VK_SHIFT] == b.shift;
}
static bool KeyHit(const KeyBind& b) { return b.valid() && g_keyPressed[b.vk] && ModsDown(b); }

static bool GameHasFocus()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

static bool IsModifierOrMouse(int vk)
{
    return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || (vk >= 0xA0 && vk <= 0xA5) || vk == VK_LWIN || vk == VK_RWIN ||
           vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
}

// ---------------------------------------------------------------------------
// wheels / gamepads: own DirectInput instance on a background thread
// ---------------------------------------------------------------------------
struct JoyDevice { GUID guid; char name[40]; IDirectInputDevice8W* dev; BYTE prev[128]; int failures; };
static std::thread g_joyThread;
static std::atomic<bool> g_joyRun{false};
static std::mutex g_joyMutex;
static std::vector<JoyBind> g_joyEvents;         // button presses since the last game frame
static std::vector<void*> g_ourJoyDevices;       // never blocked by our own hooks

static BOOL CALLBACK EnumJoy(LPCDIDEVICEINSTANCEW inst, LPVOID ctx)
{
    auto* list = (std::vector<std::pair<GUID, std::string>>*)ctx;
    char name[64] = "";
    WideCharToMultiByte(CP_UTF8, 0, inst->tszProductName, -1, name, sizeof(name), nullptr, nullptr);
    list->push_back({ inst->guidInstance, name });
    return DIENUM_CONTINUE;
}

static void JoyThread()
{
    IDirectInput8W* di = nullptr;
    if (FAILED(DirectInput8Create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W, (void**)&di, nullptr))) return;
    std::vector<JoyDevice> devs;
    double lastEnum = -100.0;

    while (g_joyRun) {
        double now = NowSeconds();
        HWND hwnd = g_gameHwnd;
        if (hwnd && now - lastEnum > 3.0) {          // pick up controllers plugged in later
            lastEnum = now;
            std::vector<std::pair<GUID, std::string>> found;
            di->EnumDevices(DI8DEVCLASS_GAMECTRL, EnumJoy, &found, DIEDFL_ATTACHEDONLY);
            for (auto& fd : found) {
                bool have = false;
                for (auto& d : devs) have |= IsEqualGUID(d.guid, fd.first) != 0;
                if (have) continue;
                IDirectInputDevice8W* dev = nullptr;
                if (FAILED(di->CreateDevice(fd.first, &dev, nullptr))) continue;
                {
                    std::lock_guard<std::mutex> lock(g_joyMutex);
                    g_ourJoyDevices.push_back(dev);
                }
                dev->SetDataFormat(&c_dfDIJoystick2);
                dev->SetCooperativeLevel(hwnd, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
                dev->Acquire();
                JoyDevice jd{};
                jd.guid = fd.first; jd.dev = dev;
                snprintf(jd.name, sizeof(jd.name), "%s", fd.second.c_str());
                devs.push_back(jd);
            }
        }
        for (size_t i = 0; i < devs.size();) {
            JoyDevice& d = devs[i];
            DIJOYSTATE2 st{};
            d.dev->Poll();
            HRESULT hr = d.dev->GetDeviceState(sizeof(st), &st);
            if (FAILED(hr)) {
                d.dev->Acquire();
                if (++d.failures > 300) {             // unplugged (~3 s): forget it
                    {
                        std::lock_guard<std::mutex> lock(g_joyMutex);
                        g_ourJoyDevices.erase(std::remove(g_ourJoyDevices.begin(), g_ourJoyDevices.end(), (void*)d.dev), g_ourJoyDevices.end());
                    }
                    d.dev->Unacquire(); d.dev->Release();
                    devs.erase(devs.begin() + i);
                    continue;
                }
                ++i; continue;
            }
            d.failures = 0;
            for (int b = 0; b < 128; ++b) {
                bool down = (st.rgbButtons[b] & 0x80) != 0;
                if (down && !(d.prev[b] & 0x80)) {
                    JoyBind e; e.device = d.guid; e.button = b;
                    snprintf(e.name, sizeof(e.name), "%s", d.name);
                    std::lock_guard<std::mutex> lock(g_joyMutex);
                    g_joyEvents.push_back(e);
                }
                d.prev[b] = st.rgbButtons[b];
            }
            ++i;
        }
        Sleep(10);
    }
    for (auto& d : devs) { d.dev->Unacquire(); d.dev->Release(); }
    {
        std::lock_guard<std::mutex> lock(g_joyMutex);
        g_ourJoyDevices.clear();
    }
    di->Release();
}

static bool JoyHit(const JoyBind& b, const std::vector<JoyBind>& events)
{
    if (!b.valid()) return false;
    for (auto& e : events) if (e.button == b.button && IsEqualGUID(e.device, b.device)) return true;
    return false;
}

// ---------------------------------------------------------------------------
// keybind capture
// ---------------------------------------------------------------------------
static std::atomic<int> g_capture{-1};
int BindCaptureTarget() { return g_capture; }

static bool CaptureIsJoy(int t)
{
    return t == BIND_RELEASE_JOY || (t >= 0 && t < BIND_OVERLAY && (t & 1));
}

bool BeginBindCapture(int target)
{
    g_capture = target;
    return true;
}

static void StoreBind(int t, const KeyBind* key, const JoyBind* joy)
{
    Settings& s = g_settings;
    if (t == BIND_OVERLAY) { if (key) s.overlayKey = *key; }
    else if (t == BIND_RELEASE_KEY) s.releaseAll.key = key ? *key : KeyBind{};
    else if (t == BIND_RELEASE_JOY) s.releaseAll.joy = joy ? *joy : JoyBind{};
    else if (t >= 0) {
        unsigned idx = (unsigned)t / 2, slot = idx / MAX_LIFT_AXLES, axle = idx % MAX_LIFT_AXLES;
        if (slot >= VEHICLE_COUNT) return;
        if (t & 1) s.axle[slot][axle].joy = joy ? *joy : JoyBind{};
        else s.axle[slot][axle].key = key ? *key : KeyBind{};
    }
    SaveSettings();
}

void ClearBind(int target)
{
    if (target == BIND_OVERLAY) return;   // the overlay always needs a key
    StoreBind(target, nullptr, nullptr);
}

static void Capture(const std::vector<JoyBind>& joyEvents)
{
    int t = g_capture;
    if (g_keyPressed[VK_ESCAPE]) { g_capture = -1; Notify("Keybind change cancelled"); return; }
    if (g_keyPressed[VK_BACK]) {
        if (t == BIND_OVERLAY) { Notify("The overlay always needs a key"); g_capture = -1; return; }
        StoreBind(t, nullptr, nullptr);
        g_capture = -1;
        Notify("Keybind removed");
        return;
    }
    if (CaptureIsJoy(t)) {
        if (joyEvents.empty()) return;
        StoreBind(t, nullptr, &joyEvents[0]);
        char txt[64]; JoyBindText(joyEvents[0], txt, sizeof(txt));
        Notify("Set to %s", txt);
        g_capture = -1;
        return;
    }
    for (int vk = 1; vk < 256; ++vk) {
        if (!g_keyPressed[vk] || IsModifierOrMouse(vk)) continue;
        KeyBind b{ vk, g_keyDown[VK_CONTROL], g_keyDown[VK_MENU], g_keyDown[VK_SHIFT] };
        StoreBind(t, &b, nullptr);
        char txt[64]; KeyBindText(b, txt, sizeof(txt));
        Notify("Set to %s", txt);
        g_capture = -1;
        return;
    }
}

// ---------------------------------------------------------------------------
// per game frame
// ---------------------------------------------------------------------------
void SetOverlayVisible(bool show)
{
    g_overlayVisible = show;
    if (show) ClipCursor(nullptr);
    else g_capture = -1;
}

void InputFrame()
{
    ReadKeyboard();
    std::vector<JoyBind> joy;
    {
        std::lock_guard<std::mutex> lock(g_joyMutex);
        joy.swap(g_joyEvents);
    }
    if (g_overlayVisible) ClipCursor(nullptr);       // some games re-lock the cursor every frame
    if (!GameHasFocus()) return;

    if (g_capture >= 0) { Capture(joy); return; }     // while choosing a key nothing else fires

    const Settings& s = g_settings;
    if (KeyHit(s.overlayKey)) SetOverlayVisible(!g_overlayVisible);
    if (KeyHit(s.releaseAll.key) || JoyHit(s.releaseAll.joy, joy)) ReleaseAll();
    for (unsigned v = 0; v < VEHICLE_COUNT; ++v) {
        const Vehicle& veh = g_vehicles[v];
        if (!veh.present) continue;
        for (unsigned i = 0; i < veh.axles.size() && i < MAX_LIFT_AXLES; ++i)
            if (KeyHit(s.axle[v][i].key) || JoyHit(s.axle[v][i].joy, joy)) ToggleAxle(v, i);
    }
}

// ---------------------------------------------------------------------------
// DirectInput hooks (shared IDirectInputDevice8 vtables, A and W):
//   mouse        overlay open  -> game gets no movement/buttons; we use them for the overlay cursor
//   keyboard     choosing a key -> game gets nothing (so the key doesn't also do something in game)
//   controllers  choosing a button -> same, except our own polling devices
// ---------------------------------------------------------------------------
typedef HRESULT (WINAPI* DiGetStateFn)(void*, DWORD, LPVOID);
typedef HRESULT (WINAPI* DiGetDataFn)(void*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
typedef HRESULT (WINAPI* DiGetCapsFn)(void*, LPDIDEVCAPS);
static const int VT_GETCAPS = 3, VT_GETSTATE = 9, VT_GETDATA = 10;

static std::mutex g_diMutex;
static std::vector<std::pair<void*, BYTE>> g_diTypes;

// overlay cursor fed from the game's own mouse reads
static std::atomic<long> g_vmDx{0}, g_vmDy{0}, g_vmWheel{0};
static std::atomic<unsigned> g_vmButtons{0};
static std::atomic<bool> g_vmSeen{false};

bool TakeVirtualMouse(float& dx, float& dy, float& wheel, unsigned& buttons)
{
    dx = (float)g_vmDx.exchange(0);
    dy = (float)g_vmDy.exchange(0);
    wheel = (float)g_vmWheel.exchange(0) / (float)WHEEL_DELTA;
    buttons = g_vmButtons;
    return g_vmSeen;
}

static BYTE DiType(void* dev, DiGetCapsFn caps)
{
    {
        std::lock_guard<std::mutex> lock(g_diMutex);
        for (auto& e : g_diTypes) if (e.first == dev) return e.second;
    }
    DIDEVCAPS dc{}; dc.dwSize = sizeof(dc);
    BYTE t = SUCCEEDED(caps(dev, &dc)) ? (BYTE)GET_DIDEVICE_TYPE(dc.dwDevType) : 0;
    std::lock_guard<std::mutex> lock(g_diMutex);
    g_diTypes.push_back({ dev, t });
    return t;
}

static bool IsOurJoy(void* dev)
{
    std::lock_guard<std::mutex> lock(g_joyMutex);
    return std::find(g_ourJoyDevices.begin(), g_ourJoyDevices.end(), dev) != g_ourJoyDevices.end();
}

// what to do with this device right now: 0 = pass, 1 = block, 2 = mouse for the overlay
static int DiPolicy(void* dev, DiGetCapsFn caps)
{
    bool overlay = g_overlayVisible;
    int cap = g_capture;
    if (!overlay && cap < 0) return 0;
    BYTE t = DiType(dev, caps);
    if (t == DI8DEVTYPE_MOUSE) return overlay ? 2 : 0;
    if (cap < 0) return 0;
    if (t == DI8DEVTYPE_KEYBOARD) return 1;
    if (t >= DI8DEVTYPE_JOYSTICK && t <= DI8DEVTYPE_SUPPLEMENTAL && CaptureIsJoy(cap) && !IsOurJoy(dev)) return 1;
    return 0;
}

template <int K> struct DiHook {
    static inline void** vt = nullptr;
    static inline DiGetStateFn origState = nullptr;
    static inline DiGetDataFn origData = nullptr;
    static inline DiGetCapsFn caps = nullptr;

    static HRESULT WINAPI State(void* self, DWORD cb, LPVOID data)
    {
        HRESULT hr = origState(self, cb, data);
        if (FAILED(hr) || !data) return hr;
        int p = DiPolicy(self, caps);
        if (p == 2 && cb >= sizeof(DIMOUSESTATE)) {
            auto* m = (DIMOUSESTATE*)data;
            g_vmDx += m->lX; g_vmDy += m->lY; g_vmWheel += m->lZ;
            unsigned b = 0;
            for (int i = 0; i < 4; ++i) if (m->rgbButtons[i] & 0x80) b |= 1u << i;
            g_vmButtons = b;
            g_vmSeen = true;
        }
        if (p) memset(data, 0, cb);
        return hr;
    }
    static HRESULT WINAPI Data(void* self, DWORD cb, LPDIDEVICEOBJECTDATA rg, LPDWORD inOut, DWORD flags)
    {
        HRESULT hr = origData(self, cb, rg, inOut, flags);   // always read, so the buffer drains
        if (FAILED(hr) || !rg || !inOut || (flags & DIGDD_PEEK)) return hr;
        int p = DiPolicy(self, caps);
        if (p == 2) {
            unsigned b = g_vmButtons;
            for (DWORD i = 0; i < *inOut; ++i) {
                auto* e = (DIDEVICEOBJECTDATA*)((BYTE*)rg + i * cb);
                if (e->dwOfs == DIMOFS_X) g_vmDx += (LONG)e->dwData;
                else if (e->dwOfs == DIMOFS_Y) g_vmDy += (LONG)e->dwData;
                else if (e->dwOfs == DIMOFS_Z) g_vmWheel += (LONG)e->dwData;
                else if (e->dwOfs >= DIMOFS_BUTTON0 && e->dwOfs <= DIMOFS_BUTTON3) {
                    unsigned bit = 1u << (e->dwOfs - DIMOFS_BUTTON0);
                    if (e->dwData & 0x80) b |= bit; else b &= ~bit;
                }
            }
            g_vmButtons = b;
            g_vmSeen = true;
        }
        if (p) *inOut = 0;
        return hr;
    }
    static void Install(void* device)
    {
        void** table = *(void***)device;
        if (vt) return;
        vt = table;
        caps = (DiGetCapsFn)table[VT_GETCAPS];
        origState = (DiGetStateFn)table[VT_GETSTATE];
        origData = (DiGetDataFn)table[VT_GETDATA];
        DWORD old;
        if (VirtualProtect(&table[VT_GETSTATE], 2 * sizeof(void*), PAGE_READWRITE, &old)) {
            table[VT_GETSTATE] = (void*)&State;
            table[VT_GETDATA] = (void*)&Data;
            VirtualProtect(&table[VT_GETSTATE], 2 * sizeof(void*), old, &old);
        }
    }
    static void Remove()
    {
        if (!vt) return;
        DWORD old;
        if (VirtualProtect(&vt[VT_GETSTATE], 2 * sizeof(void*), PAGE_READWRITE, &old)) {
            vt[VT_GETSTATE] = (void*)origState;
            vt[VT_GETDATA] = (void*)origData;
            VirtualProtect(&vt[VT_GETSTATE], 2 * sizeof(void*), old, &old);
        }
        vt = nullptr;
    }
};

static void InstallDirectInputHooks()
{
    HINSTANCE inst = GetModuleHandleA(nullptr);
    IDirectInput8W* diW = nullptr;
    if (SUCCEEDED(DirectInput8Create(inst, DIRECTINPUT_VERSION, IID_IDirectInput8W, (void**)&diW, nullptr))) {
        IDirectInputDevice8W* dev = nullptr;
        if (SUCCEEDED(diW->CreateDevice(GUID_SysMouse, &dev, nullptr))) { DiHook<0>::Install(dev); dev->Release(); }
        diW->Release();
    }
    IDirectInput8A* diA = nullptr;
    if (SUCCEEDED(DirectInput8Create(inst, DIRECTINPUT_VERSION, IID_IDirectInput8A, (void**)&diA, nullptr))) {
        IDirectInputDevice8A* dev = nullptr;
        if (SUCCEEDED(diA->CreateDevice(GUID_SysMouse, &dev, nullptr))) {
            if (*(void***)dev != DiHook<0>::vt) DiHook<1>::Install(dev);
            dev->Release();
        }
        diA->Release();
    }
    LogDebug("[axlelift] DirectInput hooks: %s", DiHook<0>::vt || DiHook<1>::vt ? "ok" : "FAILED");
}

// ---------------------------------------------------------------------------
// cursor: the game may lock / re-centre it - not while the overlay is open
// ---------------------------------------------------------------------------
static BOOL WINAPI HookClipCursor(const RECT* r)  { return ClipCursor(g_overlayVisible ? nullptr : r); }
static BOOL WINAPI HookSetCursorPos(int x, int y) { return g_overlayVisible ? TRUE : SetCursorPos(x, y); }

struct IatPatch { void** slot; void* orig; };
static std::vector<IatPatch> g_iat;

static void PatchImport(const char* dll, const char* func, void* hook)
{
    BYTE* base = (BYTE*)GetModuleHandleA(nullptr);
    void* target = (void*)GetProcAddress(GetModuleHandleA(dll), func);
    if (!target) return;
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return;
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((const char*)(base + imp->Name), dll) != 0) continue;
        for (auto th = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk); th->u1.Function; ++th) {
            if ((void*)th->u1.Function != target) continue;
            DWORD old;
            if (!VirtualProtect(&th->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return;
            g_iat.push_back({ (void**)&th->u1.Function, target });
            th->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&th->u1.Function, sizeof(void*), old, &old);
        }
    }
}

// ---------------------------------------------------------------------------
// game window: mouse messages go to the overlay while it is open
// ---------------------------------------------------------------------------
bool OverlayWindowMessage(HWND h, UINT m, WPARAM w, LPARAM l);   // overlay.cpp
static WNDPROC g_origProc = nullptr;
static HWND g_subclassed = nullptr;

static LRESULT CALLBACK GameWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (g_overlayVisible) {
        if (m == WM_SETCURSOR && LOWORD(l) == HTCLIENT) { SetCursor(nullptr); return TRUE; } // overlay draws its own
        if (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) { OverlayWindowMessage(h, m, w, l); return 0; }
    }
    return CallWindowProcW(g_origProc, h, m, w, l);
}

void HookGameWindow(HWND hwnd)
{
    g_gameHwnd = hwnd;
    if (!hwnd || g_subclassed == hwnd) return;
    g_origProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)&GameWndProc);
    if (g_origProc) g_subclassed = hwnd;
}

// ---------------------------------------------------------------------------
void InstallInputHooks()
{
    InstallDirectInputHooks();
    PatchImport("USER32.dll", "ClipCursor", (void*)&HookClipCursor);
    PatchImport("USER32.dll", "SetCursorPos", (void*)&HookSetCursorPos);
}

void RemoveInputHooks()
{
    g_overlayVisible = false;
    g_capture = -1;
    DiHook<0>::Remove();
    DiHook<1>::Remove();
    for (auto& p : g_iat) {
        DWORD old;
        if (VirtualProtect(p.slot, sizeof(void*), PAGE_READWRITE, &old)) { *p.slot = p.orig; VirtualProtect(p.slot, sizeof(void*), old, &old); }
    }
    g_iat.clear();
    if (g_subclassed && (WNDPROC)GetWindowLongPtrW(g_subclassed, GWLP_WNDPROC) == &GameWndProc)
        SetWindowLongPtrW(g_subclassed, GWLP_WNDPROC, (LONG_PTR)g_origProc);
    g_subclassed = nullptr;
}

void InputInit()
{
    g_joyRun = true;
    g_joyThread = std::thread(JoyThread);
}

void InputShutdown()
{
    g_joyRun = false;
    if (g_joyThread.joinable()) g_joyThread.join();
}
