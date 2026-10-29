// Lift Axle Extension for Euro Truck Simulator 2
// Lift each truck / trailer lift axle on its own, from an in-game overlay or keys / controller buttons.
//
// Files:
//   dllmain.cpp   SDK entry points, telemetry, multiplayer guard, logging
//   vehicles.cpp  axles, animation, game-key handover, automatic mode (reversing), steering lock
//   memscan.cpp   finding the game's lift data, diagnostics
//   input.cpp     keys, wheel/gamepad buttons, keybind capture, mouse capture for the overlay
//   overlay.cpp   Dear ImGui overlay inside the game (DirectX 11)
//   settings.cpp  axlelift.ini next to the DLL
#include "common.h"
#include <memory>

static scs_log_t g_log = nullptr;
bool  g_blocked = false;
char  g_blockedReason[128] = "";
char  g_gameVersion[32] = "";
bool  g_versionTested = false;
char  g_notice[200] = "";
double g_noticeAt = -1e9;

// ---------------------------------------------------------------------------
// logging (game thread only - the SDK log function is not meant for other threads)
// ---------------------------------------------------------------------------
static void LogV(scs_log_type_t type, const char* fmt, va_list ap)
{
    if (!g_log) return;
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    g_log(type, buf);
}

void Log(scs_log_type_t type, const char* fmt, ...) { va_list ap; va_start(ap, fmt); LogV(type, fmt, ap); va_end(ap); }
void LogInfo(const char* fmt, ...) { va_list ap; va_start(ap, fmt); LogV(SCS_LOG_TYPE_message, fmt, ap); va_end(ap); }
void LogDebug(const char* fmt, ...)
{
    if (!g_settings.verboseLog) return;
    va_list ap; va_start(ap, fmt); LogV(SCS_LOG_TYPE_message, fmt, ap); va_end(ap);
}

void Notify(const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vsnprintf(g_notice, sizeof(g_notice), fmt, ap);
    va_end(ap);
    g_noticeAt = NowSeconds();
    LogInfo("[axlelift] %s", g_notice);
}

double NowSeconds()
{
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
}

// ---------------------------------------------------------------------------
// multiplayer guard: TruckersMP (memory changes can get players banned) and
// SCS Convoy (would desync) -> the plugin never writes there
// ---------------------------------------------------------------------------
static int32_t g_mpTimeOffset = 0;
static double  g_lastMpCheck = -1e9;

static void CheckMultiplayer()
{
    double now = NowSeconds();
    if (now - g_lastMpCheck < 2.0) return;
    g_lastMpCheck = now;

    const char* reason = nullptr;
    if (GetModuleHandleA("core_ets2mp.dll") || GetModuleHandleA("core_atsmp.dll")) reason = "TruckersMP detected - the plugin is off in multiplayer";
    else if (g_mpTimeOffset != 0) reason = "Convoy multiplayer session - the plugin is off in multiplayer";

    bool blocked = reason != nullptr;
    if (blocked && !g_blocked) {
        snprintf(g_blockedReason, sizeof(g_blockedReason), "%s", reason);
        Log(SCS_LOG_TYPE_warning, "[axlelift] %s", reason);
    } else if (!blocked && g_blocked) {
        g_blockedReason[0] = 0;
        LogInfo("[axlelift] single player again - plugin active");
    }
    g_blocked = blocked;
}

// ---------------------------------------------------------------------------
// telemetry callbacks
// ---------------------------------------------------------------------------
static double g_lastFrame = 0.0;

static float FrameDt()
{
    double now = NowSeconds();
    float dt = g_lastFrame > 0.0 ? (float)(now - g_lastFrame) : 0.0f;
    g_lastFrame = now;
    return std::min(std::max(dt, 0.0f), 0.1f); // pauses / loading screens don't count
}

SCSAPI_VOID OnFrameStart(const scs_event_t, const void* const, const scs_context_t)
{
    try {
        CheckMultiplayer();
        InputFrame();
        RunCommands();
        UpdateAxles(FrameDt());
    } catch (...) {
        Log(SCS_LOG_TYPE_error, "[axlelift] error in frame start - ignored");
    }
}

SCSAPI_VOID OnFrameEnd(const scs_event_t, const void* const, const scs_context_t)
{
    try {
        ScanTick();
        UpdateAxles(FrameDt());   // write again after the game's update: less chance it undoes us
        OverlayPublish();
    } catch (...) {
        for (Vehicle& v : g_vehicles) if (!v.found) { v.cand.clear(); v.cand.shrink_to_fit(); }
        Log(SCS_LOG_TYPE_error, "[axlelift] error in frame end (out of memory?) - search reset");
    }
}

SCSAPI_VOID OnConfiguration(const scs_event_t, const void* const event_info, const scs_context_t)
{
    auto info = static_cast<const scs_telemetry_configuration_t*>(event_info);
    if (info && info->id) OnVehicleConfig(info->id, info->attributes);
}

SCSAPI_VOID OnWheelLift(const scs_string_t, const scs_u32_t index, const scs_value_t* const value, const scs_context_t context)
{
    auto* v = static_cast<Vehicle*>(context);
    if (v && value && index < MAX_WHEELS) v->wheels[index].gameLift = value->value_float.value;
}

SCSAPI_VOID OnWheelOffset(const scs_string_t, const scs_u32_t index, const scs_value_t* const value, const scs_context_t context)
{
    auto* v = static_cast<Vehicle*>(context);
    if (v && value && index < MAX_WHEELS) v->wheels[index].liftOffset = value->value_float.value;
}

SCSAPI_VOID OnWheelDeflection(const scs_string_t, const scs_u32_t index, const scs_value_t* const value, const scs_context_t context)
{
    auto* v = static_cast<Vehicle*>(context);
    if (v && value && index < MAX_WHEELS) v->wheels[index].deflection = value->value_float.value;
}

SCSAPI_VOID OnWheelGround(const scs_string_t, const scs_u32_t index, const scs_value_t* const value, const scs_context_t context)
{
    auto* v = static_cast<Vehicle*>(context);
    if (v && value && index < MAX_WHEELS) v->wheels[index].onGround = value->value_bool.value != 0;
}

SCSAPI_VOID OnWheelSteering(const scs_string_t, const scs_u32_t index, const scs_value_t* const value, const scs_context_t context)
{
    auto* v = static_cast<Vehicle*>(context);
    if (v && value && index < MAX_WHEELS) v->wheels[index].steering = value->value_float.value;
}

SCSAPI_VOID OnSpeed(const scs_string_t, const scs_u32_t, const scs_value_t* const value, const scs_context_t)
{
    if (value) g_truckSpeed = value->value_float.value;
}

SCSAPI_VOID OnGear(const scs_string_t, const scs_u32_t, const scs_value_t* const value, const scs_context_t)
{
    if (value) g_truckGear = value->value_s32.value;
}

SCSAPI_VOID OnMpOffset(const scs_string_t, const scs_u32_t, const scs_value_t* const value, const scs_context_t)
{
    g_mpTimeOffset = value ? value->value_s32.value : 0;
}

// ---------------------------------------------------------------------------
// SDK input: a "semantical" device whose inputs go straight to the game's actions of the same
// name (like Steam Input). Used to press the game's own lift-axle action: "liftaxle" (truck)
// and "liftaxlet" (trailers). No key binding needed.
// ---------------------------------------------------------------------------
static const scs_input_device_input_t g_semInputs[2] = {
    { "liftaxle", "Lift axle", SCS_VALUE_TYPE_bool },
    { "liftaxlet", "Trailer lift axle", SCS_VALUE_TYPE_bool },
};
static std::atomic<bool> g_semActive{false};
static int g_semPhase[2] = { 0, 0 };   // 0 idle, 1 send press, 2 pressed (release next frame), 3 send release

bool PressGameLift(bool trailer)
{
    int i = trailer ? 1 : 0;
    if (!g_semActive || g_semPhase[i] != 0) return false;
    g_semPhase[i] = 1;
    return true;
}

SCSAPI_VOID OnSemActive(const scs_u8_t active, const scs_context_t)
{
    g_semActive = active != 0;
    if (!active) g_semPhase[0] = g_semPhase[1] = 0;
}

SCSAPI_RESULT OnSemEvent(scs_input_event_t* const ev, const scs_u32_t flags, const scs_context_t)
{
    if (flags & SCS_INPUT_EVENT_CALLBACK_FLAG_first_in_frame)
        for (int& ph : g_semPhase) if (ph == 2) ph = 3;      // held for one frame: release now
    for (int i = 0; i < 2; ++i) {
        if (g_semPhase[i] == 1) { ev->input_index = (scs_u32_t)i; ev->value_bool.value = 1; g_semPhase[i] = 2; return SCS_RESULT_ok; }
        if (g_semPhase[i] == 3) { ev->input_index = (scs_u32_t)i; ev->value_bool.value = 0; g_semPhase[i] = 0; return SCS_RESULT_ok; }
    }
    return SCS_RESULT_not_found;
}

SCSAPI_RESULT scs_input_init(const scs_u32_t version, const scs_input_init_params_t* const params)
{
    if (version != SCS_INPUT_VERSION_1_00) return SCS_RESULT_unsupported;
    auto p = static_cast<const scs_input_init_params_v100_t*>(params);
    if (strcmp(p->common.game_id, SCS_GAME_ID_EUT2) != 0) return SCS_RESULT_unsupported;
    static scs_input_device_t dev{};   // kept alive: the game may keep referring to it
    dev.name = "axlelift";
    dev.display_name = "Lift Axle Extension";
    dev.type = SCS_INPUT_DEVICE_TYPE_semantical;
    dev.input_count = 2;
    dev.inputs = g_semInputs;
    dev.callback_context = nullptr;
    dev.input_active_callback = OnSemActive;
    dev.input_event_callback = OnSemEvent;
    if (p->register_device(&dev) != SCS_RESULT_ok) {
        if (p->common.log) p->common.log(SCS_LOG_TYPE_warning, "[axlelift] could not register the input device - the game's lift action can't be used");
        return SCS_RESULT_generic_error;
    }
    return SCS_RESULT_ok;
}

SCSAPI_VOID scs_input_shutdown(void)
{
    g_semActive = false;
}

// ---------------------------------------------------------------------------
// SDK entry points
// ---------------------------------------------------------------------------
SCSAPI_RESULT scs_telemetry_init(const scs_u32_t version, const scs_telemetry_init_params_t* const params)
{
    if (version != SCS_TELEMETRY_VERSION_1_01) return SCS_RESULT_unsupported;
    auto p = static_cast<const scs_telemetry_init_params_v101_t*>(params);
    g_log = p->common.log;

    if (strcmp(p->common.game_id, SCS_GAME_ID_EUT2) != 0) {
        Log(SCS_LOG_TYPE_warning, "[axlelift] this plugin is made for Euro Truck Simulator 2 (game id %s) - not loaded", p->common.game_id);
        return SCS_RESULT_unsupported;
    }

    LoadSettings();
    VehiclesInit();
    g_versionTested = CheckGameVersion(g_gameVersion, sizeof(g_gameVersion));

    p->register_for_event(SCS_TELEMETRY_EVENT_frame_start, OnFrameStart, nullptr);
    p->register_for_event(SCS_TELEMETRY_EVENT_frame_end, OnFrameEnd, nullptr);
    p->register_for_event(SCS_TELEMETRY_EVENT_configuration, OnConfiguration, nullptr);

    // per wheel, for the truck ("truck.wheel.*") and every trailer ("trailer.N.wheel.*")
    struct WheelChannel { const char* suffix; scs_value_type_t type; scs_telemetry_channel_callback_t cb; };
    const WheelChannel wheelChannels[] = {
        { "wheel.lift", SCS_VALUE_TYPE_float, OnWheelLift },
        { "wheel.lift.offset", SCS_VALUE_TYPE_float, OnWheelOffset },
        { "wheel.suspension.deflection", SCS_VALUE_TYPE_float, OnWheelDeflection },
        { "wheel.on_ground", SCS_VALUE_TYPE_bool, OnWheelGround },
        { "wheel.steering", SCS_VALUE_TYPE_float, OnWheelSteering },
    };
    for (unsigned s = 0; s < VEHICLE_COUNT; ++s) {
        for (const auto& wc : wheelChannels) {
            char name[64];
            if (s == 0) snprintf(name, sizeof(name), "truck.%s", wc.suffix);
            else snprintf(name, sizeof(name), "trailer.%u.%s", s - 1, wc.suffix);
            for (unsigned w = 0; w < MAX_WHEELS; ++w)
                p->register_for_channel(name, w, wc.type, SCS_TELEMETRY_CHANNEL_FLAG_none, wc.cb, &g_vehicles[s]);
        }
    }
    p->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_speed, SCS_U32_NIL, SCS_VALUE_TYPE_float, SCS_TELEMETRY_CHANNEL_FLAG_none, OnSpeed, nullptr);
    p->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_engine_gear, SCS_U32_NIL, SCS_VALUE_TYPE_s32, SCS_TELEMETRY_CHANNEL_FLAG_none, OnGear, nullptr);
    p->register_for_channel("multiplayer.time.offset", SCS_U32_NIL, SCS_VALUE_TYPE_s32, SCS_TELEMETRY_CHANNEL_FLAG_none, OnMpOffset, nullptr);

    InstallInputHooks();
    InputInit();
    OverlayInit();

    char key[32]; KeyBindText(g_settings.overlayKey, key, sizeof(key));
    LogInfo("[axlelift] Lift Axle Extension %s loaded (game %s%s) - %s opens the overlay", AXLELIFT_VERSION, g_gameVersion,
            g_versionTested ? "" : ", untested version", key);
    return SCS_RESULT_ok;
}

SCSAPI_VOID scs_telemetry_shutdown(void)
{
    ScanShutdown();          // stop a background search before the DLL goes away
    OverlayShutdown();
    InputShutdown();
    RemoveInputHooks();
    g_log = nullptr;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }

// the game looks these two up by name
#pragma comment(linker, "/EXPORT:scs_telemetry_init")
#pragma comment(linker, "/EXPORT:scs_telemetry_shutdown")
#pragma comment(linker, "/EXPORT:scs_input_init")
#pragma comment(linker, "/EXPORT:scs_input_shutdown")
