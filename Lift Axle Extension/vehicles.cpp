// vehicles: telemetry layout, axle control (animation, release, game-key handover),
// automatic mode, realism (overload protection), axle loads
#include "common.h"

Vehicle g_vehicles[VEHICLE_COUNT];
float g_truckSpeed = 0.0f;
int   g_truckGear = 0;

void VehiclesInit()
{
    for (unsigned s = 0; s < VEHICLE_COUNT; ++s) {
        Vehicle& v = g_vehicles[s];
        v = Vehicle{};
        v.slot = s;
        if (s == 0) { snprintf(v.id, sizeof(v.id), "truck"); snprintf(v.name, sizeof(v.name), "Truck"); }
        else { snprintf(v.id, sizeof(v.id), "trailer.%u", s - 1); snprintf(v.name, sizeof(v.name), "Trailer %u", s); }
    }
}

static int SlotFromConfigId(const char* id)
{
    if (!strcmp(id, "truck")) return 0;
    unsigned n;
    char extra;
    if (sscanf(id, "trailer.%u%c", &n, &extra) == 1 && n < MAX_TRAILERS) return (int)n + 1;
    return -1; // "trailer" (legacy duplicate of trailer.0) and everything else
}

// ---------------------------------------------------------------------------
// configuration: wheel layout -> axles
// ---------------------------------------------------------------------------
void OnVehicleConfig(const char* id, const scs_named_value_t* attributes)
{
    int slot = SlotFromConfigId(id);
    if (slot < 0) return;
    Vehicle& v = g_vehicles[slot];

    Wheel wheels[MAX_WHEELS];
    unsigned count = 0;
    for (const scs_named_value_t* a = attributes; a && a->name; ++a) {
        if (!strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_wheel_count))
            count = std::min<unsigned>(a->value.value_u32.value, MAX_WHEELS);
        else if (!strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_wheel_liftable) && a->index < MAX_WHEELS)
            wheels[a->index].liftable = a->value.value_bool.value != 0;
        else if (!strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_wheel_position) && a->index < MAX_WHEELS)
            wheels[a->index].posZ = a->value.value_fvector.z;
    }

    // the game re-sends configurations for unrelated reasons: keep everything if the layout is unchanged
    bool same = v.present && count == v.wheelCount;
    for (unsigned i = 0; same && i < count; ++i)
        same = wheels[i].liftable == v.wheels[i].liftable && std::fabs(wheels[i].posZ - v.wheels[i].posZ) < 0.01f;
    if (same) {
        // same layout, but the game may have rebuilt the vehicle (e.g. loading cargo) -> check it
        if (v.found) { v.verifyNeeded = true; LogDebug("[axlelift] %s re-sent by the game - checking the lift data", v.name); }
        return;
    }

    ResetTargets(v);
    v.axles.clear();
    v.axleZ.clear();
    for (unsigned i = 0; i < MAX_WHEELS; ++i) { v.wheels[i] = Wheel{}; if (i < count) v.wheels[i] = wheels[i]; }
    v.wheelCount = count;
    v.present = count > 0;
    if (!v.present) { LogDebug("[axlelift] %s removed", v.name); return; }

    // all axles, front (smaller z) to back
    for (unsigned i = 0; i < count; ++i) {
        bool known = false;
        for (float z : v.axleZ) if (std::fabs(z - v.wheels[i].posZ) <= 0.2f) known = true;
        if (!known) v.axleZ.push_back(v.wheels[i].posZ);
    }
    std::sort(v.axleZ.begin(), v.axleZ.end());

    // liftable axles
    for (unsigned k = 0; k < v.axleZ.size(); ++k) {
        Axle ax;
        for (unsigned i = 0; i < count; ++i)
            if (v.wheels[i].liftable && std::fabs(v.wheels[i].posZ - v.axleZ[k]) <= 0.2f) ax.wheels.push_back(i);
        if (ax.wheels.empty() || v.axles.size() >= MAX_LIFT_AXLES) continue;
        ax.allIndex = k;
        for (unsigned w : ax.wheels) v.wheels[w].liftAxle = (int)v.axles.size();
        v.axles.push_back(ax);
    }

    if (!v.axles.empty()) {
        v.scanArmed = true;   // finished automatically the first time the axles move with the game key
        v.lastL = -1.0f;
    }
    LogInfo("[axlelift] %s: %u wheels, %u axles, %u liftable", v.name, count, (unsigned)v.axleZ.size(), (unsigned)v.axles.size());
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
static float GameLift(const Vehicle& v, const Axle& a) { return v.wheels[a.wheels[0]].gameLift; }

float AxleUiPos(const Vehicle& v, const Axle& a) { return a.controlled ? a.pos : GameLift(v, a); }
bool  AxleUiUp(const Vehicle& v, const Axle& a)  { return a.controlled ? (a.releasing ? GameLift(v, a) > 0.5f : a.desiredUp) : GameLift(v, a) > 0.5f; }

static unsigned MemIndex(const Vehicle& v, unsigned allIndex, bool reversed)
{
    return reversed ? (unsigned)v.axleZ.size() - 1 - allIndex : allIndex;
}

// load on an axle (newtons in memory -> tonnes); < 0 = unknown
float AxleLoadT(const Vehicle& v, unsigned allIndex)
{
    if (!v.loadArray || allIndex >= v.axleZ.size()) return -1.0f;
    float n;
    if (!ReadMem(v.loadArray + 4 * MemIndex(v, allIndex, v.loadReversed), &n, 4) || !std::isfinite(n) || n < -1000.0f) return -1.0f;
    return std::max(n, 0.0f) / 9.81f / 1000.0f;
}

static bool VehicleLoads(const Vehicle& v, float& total)
{
    total = 0.0f;
    for (unsigned k = 0; k < v.axleZ.size(); ++k) {
        float t = AxleLoadT(v, k);
        if (t < 0.0f) return false;
        total += t;
    }
    return total > 0.05f;
}

// axles on the ground, optionally pretending one more axle is lifted
static unsigned GroundAxles(const Vehicle& v, int extraLifted)
{
    unsigned down = (unsigned)v.axleZ.size();
    for (unsigned i = 0; i < v.axles.size(); ++i)
        if (AxleUiUp(v, v.axles[i]) || (int)i == extraLifted) --down;
    return std::max(down, 1u);
}

// remember where a plugin move starts, to check that the real wheel follows (see UpdateAxles)
static void StartMove(const Vehicle& v, Axle& a)
{
    a.checking = true;
    a.checkFrom = a.pos;
    a.checkOffset = v.wheels[a.wheels[0]].liftOffset;
}

static void TakeControl(Vehicle& v, Axle& a)
{
    if (a.controlled) return;
    a.pos = GameLift(v, a);
    a.desiredUp = a.pos > 0.5f;
    a.controlled = true;
    a.releasing = false;
    a.lastGameLift = GameLift(v, a);
}

// ---------------------------------------------------------------------------
// player actions
// ---------------------------------------------------------------------------
void ToggleAxle(unsigned slot, unsigned axle)
{
    if (slot >= VEHICLE_COUNT) return;
    Vehicle& v = g_vehicles[slot];
    if (!v.present || axle >= v.axles.size()) return;
    if (g_blocked) { Notify("Disabled: %s", g_blockedReason); return; }
    if (!v.found) { Notify("%s: lift or lower the axles once with the game key to finish setup", v.name); return; }
    if (ProbeActive()) { Notify("One moment - checking the lift data"); return; }

    Axle& a = v.axles[axle];
    bool wantUp = !AxleUiUp(v, a);

    if (wantUp && g_settings.realism) {
        float total;
        if (VehicleLoads(v, total)) {
            float after = total / (float)GroundAxles(v, (int)axle);
            if (after > g_settings.maxAxleLoadT) {
                Notify("%s axle %u stays down: the other axles would carry %.1f t each (limit %.1f t)",
                       v.name, axle + 1, after, g_settings.maxAxleLoadT);
                return;
            }
        }
    }
    TakeControl(v, a);
    a.desiredUp = wantUp;
    a.releasing = false;
    a.autoSince = 0.0;
    StartMove(v, a);
    LogDebug("[axlelift] %s axle %u -> %s", v.name, axle + 1, wantUp ? "UP" : "DOWN");
}

void ReleaseVehicle(unsigned slot)
{
    if (slot >= VEHICLE_COUNT) return;
    Vehicle& v = g_vehicles[slot];
    for (auto& a : v.axles) if (a.controlled) a.releasing = true;
}

void ReleaseAll()
{
    for (unsigned s = 0; s < VEHICLE_COUNT; ++s) ReleaseVehicle(s);
    Notify("Axles return to the game's position, then the game takes over");
}

// ---------------------------------------------------------------------------
// automatic mode + realism (needs axle loads, reverse rule only needs the gear)
// ---------------------------------------------------------------------------
static void Automation(Vehicle& v, double now)
{
    if (ProbeActive()) return;
    const Settings& s = g_settings;
    float total = 0.0f;
    bool loads = VehicleLoads(v, total);
    bool changedThisFrame = false;

    // rear axles first: they are usually the ones lifted
    for (int i = (int)v.axles.size() - 1; i >= 0; --i) {
        Axle& a = v.axles[i];
        bool up = AxleUiUp(v, a);

        // realism: an axle that is up while the others are overloaded comes down
        if (s.realism && s.realismAutoLower && loads && up) {
            float perAxle = total / (float)GroundAxles(v, -1);
            if (perAxle > s.maxAxleLoadT) {
                if (a.realismSince == 0.0) a.realismSince = now;
                if (now - a.realismSince > 2.0 && !changedThisFrame) {
                    TakeControl(v, a);
                    a.desiredUp = false; a.releasing = false;
                    StartMove(v, a);
                    a.realismSince = 0.0;
                    changedThisFrame = true;
                    Notify("%s axle %d lowered: %.1f t per axle is above the %.1f t limit", v.name, i + 1, perAxle, s.maxAxleLoadT);
                }
                continue;
            }
        }
        a.realismSince = 0.0;

        if (!s.axleAuto[v.slot][i]) { a.autoSince = 0.0; continue; }

        // what should this axle do?
        int want = -1; // -1 = no opinion, 0 = down, 1 = up
        if (g_truckGear < 0 && s.autoReverse != ReverseRule::None) {
            want = s.autoReverse == ReverseRule::Lift ? 1 : 0;
        } else if (loads) {
            if (up) want = (total / (float)GroundAxles(v, -1) > s.autoLowerAboveT) ? 0 : 1;
            else    want = (total / (float)GroundAxles(v, i) <= s.autoLiftBelowT) ? 1 : 0;
        }
        if (want == 1 && s.realism && loads && total / (float)GroundAxles(v, i) > s.maxAxleLoadT) want = 0;
        if (want < 0 || (want == 1) == up) { a.autoSince = 0.0; continue; }

        // condition must hold for a moment (no flapping over bumps); one change per vehicle per frame
        if (a.autoSince == 0.0) a.autoSince = now;
        if (now - a.autoSince < 3.0 || changedThisFrame) continue;
        TakeControl(v, a);
        a.desiredUp = want == 1;
        StartMove(v, a);
        a.releasing = false;
        a.autoSince = 0.0;
        changedThisFrame = true;
        LogDebug("[axlelift] auto: %s axle %d -> %s", v.name, i + 1, want ? "UP" : "DOWN");
    }
}

// ---------------------------------------------------------------------------
// per frame
// ---------------------------------------------------------------------------
static bool WriteVehicle(Vehicle& v)
{
    // axle list: sanity check first, then write controlled axles
    if (v.axleList) {
        bool valid = true;
        for (unsigned k = 0; k < v.axleZ.size(); ++k) {
            float x; if (!ReadMem(v.axleList + 4 * k, &x, 4) || !(x >= -0.01f && x <= 1.01f)) valid = false;
        }
        if (!valid) {
            Log(SCS_LOG_TYPE_warning, "[axlelift] %s: lift memory no longer valid - searching again", v.name);
            RescanVehicle(v.slot);
            return false;
        }
        for (auto& a : v.axles)
            if (a.controlled) WriteFloat(v.axleList + 4 * MemIndex(v, a.allIndex, v.listReversed), a.pos);
    }
    for (size_t i = 0; i < v.wheelTargets.size();) {
        auto& t = v.wheelTargets[i];
        uint64_t vt = 0; uint32_t idx = 0xFFFFFFFF;
        if (!ReadMem(t.obj, &vt, 8) || !ReadMem(t.obj + 0x20, &idx, 4) || vt != t.vtbl || idx != t.wheel) {
            v.wheelTargets.erase(v.wheelTargets.begin() + i);
            continue;
        }
        int ax = v.wheels[t.wheel].liftAxle;
        if (ax >= 0 && v.axles[ax].controlled) {
            WriteFloat(t.obj + 0x80, v.axles[ax].pos);
            WriteFloat(t.obj + 0x150, v.axles[ax].pos);
        }
        ++i;
    }
    return true;
}

void UpdateAxles(float dt)
{
    double now = NowSeconds();
    float step = dt / std::max(g_settings.liftSeconds, 0.05f);

    for (unsigned s = 0; s < VEHICLE_COUNT; ++s) {
        Vehicle& v = g_vehicles[s];
        if (!v.present || !v.found) continue;

        if (g_blocked) {
            // multiplayer: put everything back to the game's state once, then never write
            for (auto& a : v.axles) if (a.controlled) { a.pos = GameLift(v, a); }
            WriteVehicle(v);
            for (auto& a : v.axles) a.controlled = false;
            continue;
        }

        for (unsigned i = 0; i < v.axles.size(); ++i) {
            Axle& a = v.axles[i];
            float gl = GameLift(v, a);

            // the game's own lift key moved this axle: the game wins
            if (a.controlled && !a.releasing && std::fabs(gl - a.lastGameLift) > 0.02f) {
                a.controlled = false;
                if (g_settings.axleAuto[s][i]) {
                    g_settings.axleAuto[s][i] = false;
                    SaveSettings();
                    Notify("%s axle %u: game lift key used - automatic mode switched off for this axle", v.name, i + 1);
                } else {
                    Notify("%s axle %u: game lift key used - the game controls it again", v.name, i + 1);
                }
            }
            a.lastGameLift = gl;
            if (!a.controlled) continue;

            float target = a.releasing ? gl : (a.desiredUp ? 1.0f : 0.0f);
            if (a.pos < target) a.pos = std::min(target, a.pos + step);
            else if (a.pos > target) a.pos = std::max(target, a.pos - step);
        }

        Automation(v, now);
        if (!WriteVehicle(v)) continue;

        // does the real wheel follow our moves? If the game rebuilt the vehicle (e.g. cargo loaded),
        // we'd be writing into memory it no longer uses. Only possible if the game reports wheel heights.
        for (unsigned w = 0; w < v.wheelCount; ++w)
            if (v.wheels[w].liftable && v.wheels[w].liftOffset > 0.01f) v.offsetSeen = true;
        bool lost = false;
        for (auto& a : v.axles) {
            if (!a.checking || !a.controlled) { a.checking = false; continue; }
            if (std::fabs(a.pos - a.checkFrom) < 0.5f) continue;
            a.checking = false;
            if (v.offsetSeen && std::fabs(v.wheels[a.wheels[0]].liftOffset - a.checkOffset) < 0.01f) lost = true;
        }
        if (lost) { DataLost(s); continue; }

        for (auto& a : v.axles)
            if (a.controlled && a.releasing && std::fabs(a.pos - GameLift(v, a)) < 0.001f) {
                a.controlled = false;
                a.releasing = false;
            }
    }
}

// ---------------------------------------------------------------------------
// identical trailers moving together can't be told apart in memory: let the player swap
// ---------------------------------------------------------------------------
bool CanSwapWithNext(unsigned slot)
{
    if (slot == 0 || slot + 1 >= VEHICLE_COUNT) return false;
    const Vehicle& a = g_vehicles[slot];
    const Vehicle& b = g_vehicles[slot + 1];
    if (!a.present || !b.present || !a.found || !b.found || a.axleZ.size() != b.axleZ.size() || a.axles.size() != b.axles.size()) return false;
    for (size_t i = 0; i < a.axles.size(); ++i) if (a.axles[i].allIndex != b.axles[i].allIndex) return false;
    return a.uncertain || b.uncertain;
}

void SwapWithNext(unsigned slot)
{
    if (!CanSwapWithNext(slot)) return;
    Vehicle& a = g_vehicles[slot];
    Vehicle& b = g_vehicles[slot + 1];
    ReleaseVehicle(slot); ReleaseVehicle(slot + 1);
    for (auto& x : a.axles) x.controlled = false;
    for (auto& x : b.axles) x.controlled = false;
    std::swap(a.axleList, b.axleList);
    std::swap(a.listReversed, b.listReversed);
    std::swap(a.wheelTargets, b.wheelTargets);
    std::swap(a.loadArray, b.loadArray);
    std::swap(a.loadReversed, b.loadReversed);
    Notify("%s and %s swapped", a.name, b.name);
}
