// Lift Axle Extension for Euro Truck Simulator 2 - shared declarations
#pragma once

#define _CRT_SECURE_NO_WARNINGS
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <shlobj.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <mutex>
#include <atomic>
#include <thread>
#include <algorithm>
#include <memory>

#pragma comment(lib, "psapi.lib")

#include "scssdk_telemetry.h"
#include "scssdk_telemetry_event.h"
#include "eurotrucks2/scssdk_eut2.h"
#include "eurotrucks2/scssdk_telemetry_eut2.h"
#include "common/scssdk_telemetry_common_configs.h"
#include "common/scssdk_telemetry_common_channels.h"
#include "common/scssdk_telemetry_truck_common_channels.h"
#include "common/scssdk_telemetry_trailer_common_channels.h"

#define AXLELIFT_VERSION "1.0.0-beta4"

// ---------------------------------------------------------------------------
// limits
// ---------------------------------------------------------------------------
static const unsigned MAX_WHEELS = 16;
static const unsigned MAX_LIFT_AXLES = 8;     // per vehicle
static const unsigned MAX_TRAILERS = 10;      // SDK reports trailer.0 .. trailer.9
static const unsigned VEHICLE_COUNT = 1 + MAX_TRAILERS; // slot 0 = truck, 1.. = trailer.0 ..

// ---------------------------------------------------------------------------
// logging (main.cpp)
// ---------------------------------------------------------------------------
void Log(scs_log_type_t type, const char* fmt, ...);
void LogInfo(const char* fmt, ...);
void LogDebug(const char* fmt, ...);   // only written when "verbose log" is enabled
void Notify(const char* fmt, ...);     // short message shown in the overlay (and logged); game thread only
extern char   g_notice[200];
extern double g_noticeAt;
double NowSeconds();

// overwrite before freeing: copies of game memory / addresses must not linger in freed heap
// (they would show up as false hits in the diagnostics pointer search)
template <typename T> inline void Wipe(std::vector<T>& v)
{
    if (!v.empty()) memset((void*)v.data(), 0, v.size() * sizeof(T));
    v.clear();
    v.shrink_to_fit();
}

// ---------------------------------------------------------------------------
// bindings + settings (settings.cpp)
// ---------------------------------------------------------------------------
struct KeyBind {
    int vk = 0;
    bool ctrl = false, alt = false, shift = false;
    bool valid() const { return vk != 0; }
};

struct JoyBind {
    GUID device{};          // DirectInput instance GUID of the wheel / gamepad
    int button = -1;
    char name[40] = "";     // product name, for display only
    bool valid() const { return button >= 0; }
};

struct ActionBind {
    KeyBind key;
    JoyBind joy;
};

enum class ReverseRule : int { None = 0, Lower = 1, Lift = 2 };

struct Settings {
    float liftSeconds = 2.0f;
    KeyBind overlayKey{ 'O', false, true, false };
    bool showLoads = false;

    // defaults follow EU limits: 10 t single axle, 9 t per axle in a tandem, 8 t in a tridem;
    // real lift-axle controls lower the axle before the legal limit is reached
    bool realism = false;            // protect remaining axles from overload
    float maxAxleLoadT = 9.0f;       // per axle on the ground
    bool realismAutoLower = true;    // lower again if the limit is exceeded while lifted

    float autoLiftBelowT = 7.0f;     // auto mode: lift if the other axles stay below this
    float autoLowerAboveT = 8.5f;    // auto mode: lower if the axles on the ground exceed this
    ReverseRule autoReverse = ReverseRule::None;

    float overlayMouseSpeed = 1.0f;  // overlay cursor speed (it moves with the raw mouse data)
    bool verboseLog = false;
    bool showDebug = false;

    ActionBind releaseAll;
    ActionBind axle[VEHICLE_COUNT][MAX_LIFT_AXLES];
    bool axleAuto[VEHICLE_COUNT][MAX_LIFT_AXLES] = {};
};

extern Settings g_settings;           // owned by the game thread; the overlay gets copies
void LoadSettings();
void SaveSettings();
void KeyBindText(const KeyBind& b, char* out, size_t n);
void JoyBindText(const JoyBind& b, char* out, size_t n);
const char* VehicleSlotKey(unsigned slot);   // "truck", "trailer1", ...

// ---------------------------------------------------------------------------
// vehicles (vehicles.cpp)
// ---------------------------------------------------------------------------
struct Wheel {
    bool liftable = false;
    float posZ = 0.0f;
    float gameLift = 0.0f;    // telemetry: the game's own lift state for this wheel (0..1)
    float liftOffset = 0.0f;  // telemetry: how far the wheel is actually raised (m)
    float deflection = 0.0f;  // telemetry: suspension deflection (m)
    bool  onGround = true;    // telemetry
    int liftAxle = -1;        // index into Vehicle::axles, -1 = not liftable
};

struct Axle {
    std::vector<unsigned> wheels;
    unsigned allIndex = 0;        // position among ALL axles of the vehicle
    bool controlled = false;      // plugin writes this axle
    bool desiredUp = false;
    bool releasing = false;       // moving back to the game's state, then hand over
    float pos = 0.0f;             // position we write (0 down .. 1 up)
    float lastGameLift = 0.0f;    // to notice the game's own lift key
    double autoSince = 0.0;       // auto mode: condition stable since
    double realismSince = 0.0;    // realism: overload since
    bool  checking = false;       // a plugin move is running: does the real wheel follow?
    float checkFrom = 0.0f;       // position where that move started
    float checkOffset = 0.0f;     // telemetry wheel height at that moment
};

struct WheelTarget { uintptr_t obj; uint64_t vtbl; unsigned wheel; };

struct Vehicle {
    unsigned slot = 0;
    char id[16] = "";             // SDK id: "truck", "trailer.0", ...
    char name[24] = "";           // UI: "Truck", "Trailer 1", ...
    bool present = false;
    Wheel wheels[MAX_WHEELS];
    unsigned wheelCount = 0;
    std::vector<Axle> axles;      // liftable axles, front to back
    std::vector<float> axleZ;     // all axles, front to back

    // memory (memscan.cpp)
    std::vector<uintptr_t> cand;
    bool scanArmed = false, scanDone = false;
    float lastL = -1.0f;
    bool found = false;
    unsigned detectFails = 0;     // lift movements seen without recognising the data
    bool uncertain = false;       // several identical trailers matched - assignment may be swapped
    std::vector<std::pair<uintptr_t, bool>> altLists;  // all matching lift lists (identical trailers)
    bool probeNeeded = false;     // find out which list is ours by a tiny test movement
    bool verifyNeeded = false;    // the game re-sent this vehicle (e.g. cargo loaded): is our data still live?
    bool offsetSeen = false;      // the game reports wheel heights for this vehicle (needed for the checks)
    uintptr_t axleList = 0;       // one float per axle: lift position
    bool listReversed = false;    // axle list is rear-to-front
    std::vector<WheelTarget> wheelTargets;
    uintptr_t loadArray = 0;      // one float per axle: load in newtons (0 = not found yet)
    bool loadReversed = false;

    // load finder
    std::vector<uint8_t> loadSnap;
    uintptr_t loadSnapBase = 0;
    std::vector<float> loadSnapPos;
    double loadSnapAt = 0.0;
};

extern Vehicle g_vehicles[VEHICLE_COUNT];
extern float g_truckSpeed;            // m/s, negative when reversing
extern int   g_truckGear;             // < 0 = reverse
extern bool  g_blocked;               // multiplayer: plugin does not write
extern char  g_blockedReason[128];
extern char  g_gameVersion[32];
extern bool  g_versionTested;

void VehiclesInit();
void OnVehicleConfig(const char* id, const scs_named_value_t* attributes);
void ToggleAxle(unsigned slot, unsigned axle);
void ReleaseVehicle(unsigned slot);
void ReleaseAll();
void SwapWithNext(unsigned slot);     // identical trailers: swap which memory belongs to which
bool CanSwapWithNext(unsigned slot);
void UpdateAxles(float dt);           // animation, game-key detection, auto + realism, writing
float AxleLoadT(const Vehicle& v, unsigned allIndex);    // tonnes, < 0 = unknown
float AxleUiPos(const Vehicle& v, const Axle& a);
bool  AxleUiUp(const Vehicle& v, const Axle& a);

// ---------------------------------------------------------------------------
// memory (memscan.cpp)
// ---------------------------------------------------------------------------
bool ReadMem(uintptr_t addr, void* out, size_t n);
bool WriteFloat(uintptr_t addr, float v);
void ScanTick();                      // per frame: arm / scan / refine / detect
void RescanVehicle(unsigned slot);
void DataLost(unsigned slot);         // our memory no longer drives this vehicle: set up again
void ResetTargets(Vehicle& v);
bool ProbeActive();                   // telling identical trailers apart right now
void WriteDiagnostics();              // research export (debug section)
bool CheckGameVersion(char* versionOut, size_t n);

// ---------------------------------------------------------------------------
// input (input.cpp)
// ---------------------------------------------------------------------------
extern std::atomic<bool> g_overlayVisible;
extern HWND g_gameHwnd;

void InputInit();
void InputShutdown();
void InputFrame();                    // game thread: keyboard + joystick edges, hotkeys, bind capture
void SetOverlayVisible(bool show);
bool BeginBindCapture(int target);    // target encoding: see BindTarget*
int  BindCaptureTarget();             // -1 = none
void ClearBind(int target);
bool TakeVirtualMouse(float& dx, float& dy, float& wheel, unsigned& buttons); // overlay cursor from the game's mouse reads
void InstallInputHooks();
void RemoveInputHooks();
void HookGameWindow(HWND hwnd);       // window subclass for mouse handling (overlay.cpp calls this)

// bind targets
static const int BIND_OVERLAY = 1000;
static const int BIND_RELEASE_KEY = 1001;
static const int BIND_RELEASE_JOY = 1002;
inline int BindAxleKey(unsigned slot, unsigned axle) { return (int)(slot * MAX_LIFT_AXLES + axle) * 2; }
inline int BindAxleJoy(unsigned slot, unsigned axle) { return (int)(slot * MAX_LIFT_AXLES + axle) * 2 + 1; }

// ---------------------------------------------------------------------------
// overlay (overlay.cpp)
// ---------------------------------------------------------------------------
void OverlayInit();
void OverlayShutdown();
void OverlayPublish();                // game thread: copy state for the UI

// commands from the UI (render thread) to the game thread
enum class CmdType { ToggleAxle, ReleaseVehicle, ReleaseAll, Rescan, SetAuto, BindCapture, ClearBind,
                     ApplySettings, Diagnostics, SwapTrailers, CloseOverlay };
struct Command {
    CmdType type;
    unsigned slot = 0, axle = 0;
    int value = 0;
    Settings settings;                // ApplySettings only
};
void PushCommand(const Command& c);
void RunCommands();                   // game thread
