// memory: find each vehicle's lift data, find axle loads, game version, research export
//
// Layout found on ETS2 1.61.1.9 (checked structurally at run time, never trusted blindly):
//   axle list    one float per axle (0 = down, 1 = up), front to back; drives the physics
//   wheel object [obj+0x00] vtable inside eurotrucks2.exe, [obj+0x20] u32 wheel index,
//                lift float at obj+0x80 and obj+0x150 (visual / suspension)
//   load array   one float per axle, newtons; found by watching loads move when an axle lifts
#include "common.h"

#pragma comment(lib, "version.lib")

static const char* kTestedVersions[] = { "1.61.1.9" };

// ---------------------------------------------------------------------------
// safe memory access (ReadProcessMemory on ourselves fails cleanly instead of crashing)
// ---------------------------------------------------------------------------
bool ReadMem(uintptr_t addr, void* out, size_t n)
{
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)addr, out, n, &got) && got == n;
}

bool WriteFloat(uintptr_t addr, float v)
{
    SIZE_T n = 0;
    return WriteProcessMemory(GetCurrentProcess(), (LPVOID)addr, &v, 4, &n) && n == 4;
}

static uintptr_t g_exeBase = 0, g_exeEnd = 0;
static bool InExe(uint64_t p)
{
    if (!g_exeBase) {
        MODULEINFO mi{};
        GetModuleInformation(GetCurrentProcess(), GetModuleHandleA(nullptr), &mi, sizeof(mi));
        g_exeBase = (uintptr_t)mi.lpBaseOfDll;
        g_exeEnd = g_exeBase + mi.SizeOfImage;
    }
    return p >= g_exeBase && p < g_exeEnd;
}

// walk all live (resident) read/write memory in runs of consecutive pages
template <typename F> static void ForEachResidentRun(F&& fn)
{
    SYSTEM_INFO si; GetSystemInfo(&si);
    const size_t PAGE = si.dwPageSize ? si.dwPageSize : 4096;
    const size_t BATCH = 256;
    std::vector<PSAPI_WORKING_SET_EX_INFORMATION> ws(BATCH);
    uintptr_t a = (uintptr_t)si.lpMinimumApplicationAddress, end = (uintptr_t)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    while (a < end && VirtualQuery((LPCVOID)a, &mbi, sizeof(mbi))) {
        uintptr_t base = (uintptr_t)mbi.BaseAddress;
        size_t size = mbi.RegionSize;
        bool ok = mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
                  (mbi.Protect & 0xFF) == PAGE_READWRITE && !(mbi.Protect & PAGE_GUARD);
        for (size_t pos = 0; ok && pos < size; pos += BATCH * PAGE) {
            size_t pages = std::min(BATCH, (size - pos) / PAGE);
            for (size_t i = 0; i < pages; ++i) ws[i].VirtualAddress = (PVOID)(base + pos + i * PAGE);
            bool known = QueryWorkingSetEx(GetCurrentProcess(), ws.data(), (DWORD)(pages * sizeof(ws[0]))) != 0;
            size_t runStart = 0, runLen = 0;
            for (size_t i = 0; i <= pages; ++i) {
                bool valid = i < pages && (!known || ws[i].VirtualAttributes.Valid);
                if (valid) { if (!runLen) runStart = i; ++runLen; continue; }
                if (runLen && !fn(base + pos + runStart * PAGE, runLen * PAGE)) return;
                runLen = 0;
            }
        }
        a = base + size;
    }
}

// ---------------------------------------------------------------------------
// scanning
// ---------------------------------------------------------------------------
static const size_t MAX_CANDIDATES = 20000000;

static bool FirstScan(Vehicle& v, float target, float tol)
{
    Wipe(v.cand);
    std::vector<uint8_t> buf(1 << 20);
    bool overflow = false;
    ForEachResidentRun([&](uintptr_t addr, size_t n) {
        if (n > buf.size()) buf.resize(n);
        if (!ReadMem(addr, buf.data(), n)) return true;
        for (size_t off = 0; off + 4 <= n; off += 4) {
            float f; memcpy(&f, &buf[off], 4);
            if (std::fabs(f - target) < tol) {
                v.cand.push_back(addr + off);
                if (v.cand.size() >= MAX_CANDIDATES) { overflow = true; return false; }
            }
        }
        return true;
    });
    Wipe(buf);
    if (overflow) Wipe(v.cand);
    return !overflow;
}

static void Refine(Vehicle& v, float target, float tol)
{
    std::vector<uintptr_t> keep;
    keep.reserve(v.cand.size() / 4 + 16);
    uint8_t page[4096];
    uintptr_t cur = 0; bool ok = false;
    for (uintptr_t ad : v.cand) {
        uintptr_t pg = ad & ~(uintptr_t)0xFFF;
        if (pg != cur || !cur) { cur = pg; ok = ReadMem(pg, page, sizeof(page)); }
        if (!ok) continue;
        float f; memcpy(&f, &page[ad - pg], 4);
        if (std::fabs(f - target) < tol) keep.push_back(ad);
    }
    v.cand.swap(keep);
    v.cand.shrink_to_fit();
    Wipe(keep);
}

void ResetTargets(Vehicle& v)
{
    Wipe(v.cand);
    v.scanArmed = v.scanDone = false;
    v.found = v.uncertain = false;
    v.axleList = 0; v.listReversed = false;
    Wipe(v.wheelTargets);
    Wipe(v.altLists);
    v.probeNeeded = false;
    v.verifyNeeded = false;
    v.loadArray = 0; v.loadReversed = false;
    Wipe(v.loadSnap); v.loadSnapPos.clear(); v.loadSnapBase = 0; v.loadSnapAt = 0.0;
    for (auto& a : v.axles) { a.controlled = false; a.releasing = false; }
}

void RescanVehicle(unsigned slot)
{
    if (slot >= VEHICLE_COUNT) return;
    Vehicle& v = g_vehicles[slot];
    ResetTargets(v);
    if (!v.axles.empty()) { v.scanArmed = true; v.lastL = -1.0f; }
}

void DataLost(unsigned slot)
{
    if (slot >= VEHICLE_COUNT) return;
    Vehicle& v = g_vehicles[slot];
    Log(SCS_LOG_TYPE_warning, "[axlelift] %s: the game no longer uses the old lift data (vehicle rebuilt)", v.name);
    RescanVehicle(slot);
    Notify("%s changed (cargo loaded?) - lift or lower the axles once with the game key", v.name);
}

static bool ClaimedByOther(const Vehicle& self, uintptr_t addr)
{
    for (const Vehicle& o : g_vehicles) {
        if (&o == &self || !o.found) continue;
        if (o.axleList && addr >= o.axleList && addr < o.axleList + 4 * o.axleZ.size()) return true;
        for (auto& t : o.wheelTargets) if (addr >= t.obj && addr < t.obj + 0x200) return true;
    }
    return false;
}

static void Detect(Vehicle& v, float L)
{
    std::sort(v.cand.begin(), v.cand.end());
    auto inCand = [&](uintptr_t a) { return std::binary_search(v.cand.begin(), v.cand.end(), a); };
    std::vector<WheelTarget> wheels;
    std::vector<std::pair<uintptr_t, bool>> lists;

    for (uintptr_t c : v.cand) {
        // wheel object?
        uint64_t vt = 0; uint32_t idx = 0; float second = -1.0f;
        if (ReadMem(c - 0x80, &vt, 8) && ReadMem(c - 0x60, &idx, 4) && ReadMem(c + 0xD0, &second, 4) &&
            InExe(vt) && idx < v.wheelCount && v.wheels[idx].liftable && std::fabs(second - L) < 0.01f) {
            bool dup = false;
            for (auto& t : wheels) dup |= t.obj == c - 0x80;
            if (!dup) wheels.push_back({ c - 0x80, vt, idx });
        }
        // values inside a wheel object (first or second copy) are never the axle list
        uint64_t vt2 = 0;
        if ((ReadMem(c - 0x80, &vt2, 8) && InExe(vt2)) || (ReadMem(c - 0x150, &vt2, 8) && InExe(vt2))) continue;
        // start of the axle list? (both orders: front-to-back and back-to-front)
        for (int rev = 0; rev < 2; ++rev) {
            uintptr_t base = c - 4 * (rev ? (unsigned)v.axleZ.size() - 1 - v.axles[0].allIndex : v.axles[0].allIndex);
            bool ok = true;
            for (auto& ax : v.axles) {
                unsigned mi = rev ? (unsigned)v.axleZ.size() - 1 - ax.allIndex : ax.allIndex;
                if (!inCand(base + 4 * mi)) ok = false;
            }
            for (unsigned k = 0; ok && k < v.axleZ.size(); ++k) {
                bool lift = false;
                for (auto& ax : v.axles) lift |= (rev ? (unsigned)v.axleZ.size() - 1 - ax.allIndex : ax.allIndex) == k;
                float x; if (!ReadMem(base + 4 * k, &x, 4) || std::fabs(x - (lift ? L : 0.0f)) > 0.01f) ok = false;
            }
            if (!ok) continue;
            bool dup = false;
            for (auto& l : lists) dup |= l.first == base;
            if (!dup) lists.push_back({ base, rev != 0 });
        }
    }

    if (lists.empty()) {
        ++v.detectFails;
        Log(SCS_LOG_TYPE_warning, "[axlelift] %s: %zu candidates, no known lift layout found (lift %.2f)", v.name, v.cand.size(), L);
        return;
    }
    // identical trailers lifting together all match: take one nobody else has yet, and let the
    // probe (below) find out which list really belongs to which trailer
    size_t pick = 0;
    for (size_t i = 0; i < lists.size(); ++i) if (!ClaimedByOther(v, lists[i].first)) { pick = i; break; }
    v.axleList = lists[pick].first;
    v.listReversed = lists[pick].second;
    v.uncertain = lists.size() > 1;
    v.altLists = lists;
    v.probeNeeded = lists.size() > 1;
    for (auto& t : wheels) if (ClaimedByOther(v, t.obj + 0x80)) { wheels.clear(); v.uncertain = true; break; }
    // several identical trailers moving together: wheel objects can't be told apart -> use only the
    // axle list (physics); the overlay offers "swap" if the assignment turns out wrong
    unsigned liftWheels = 0;
    for (unsigned i = 0; i < v.wheelCount; ++i) liftWheels += v.wheels[i].liftable ? 1 : 0;
    if (wheels.size() > liftWheels) { v.uncertain = true; wheels.clear(); }
    v.wheelTargets = wheels;
    v.found = true;
    Wipe(v.cand);
    LogInfo("[axlelift] %s: ready (lift data %p%s, %zu wheel objects%s)", v.name, (void*)v.axleList,
            v.listReversed ? ", rear first" : "", v.wheelTargets.size(), v.uncertain ? ", several matches" : "");
    Notify("%s: lift axles ready", v.name);
}

// ---------------------------------------------------------------------------
// axle loads: snapshot memory near the lift data, compare after the axles changed
// ---------------------------------------------------------------------------
static const size_t LOAD_WINDOW = 256 * 1024;

static void LoadSnapshot(Vehicle& v, std::vector<uint8_t>& out, uintptr_t& base)
{
    base = (v.axleList - LOAD_WINDOW) & ~(uintptr_t)0xFFF;
    out.assign(2 * LOAD_WINDOW, 0);
    for (size_t off = 0; off < out.size(); off += 4096)       // unreadable pages stay zero
        ReadMem(base + off, &out[off], 4096);
}

static bool AxlesAtRest(const Vehicle& v, std::vector<float>& pos)
{
    pos.clear();
    for (auto& a : v.axles) {
        float p = AxleUiPos(v, a);
        if (p > 0.001f && p < 0.999f) return false;
        pos.push_back(p);
    }
    return true;
}

static void FindLoads(Vehicle& v, double now)
{
    if (v.loadArray || !v.axleList) return;
    std::vector<float> pos;
    if (!AxlesAtRest(v, pos)) { v.loadSnapAt = 0.0; return; }
    if (v.loadSnapAt == 0.0) { v.loadSnapAt = now; return; }
    if (now - v.loadSnapAt < 1.5) return;                 // let the suspension settle

    if (v.loadSnap.empty()) {                             // first picture
        LoadSnapshot(v, v.loadSnap, v.loadSnapBase);
        v.loadSnapPos = pos;
        return;
    }
    if (pos == v.loadSnapPos) return;                     // nothing changed yet

    std::vector<uint8_t> now2; uintptr_t base2;
    LoadSnapshot(v, now2, base2);
    if (base2 != v.loadSnapBase) { Wipe(v.loadSnap); v.loadSnap.swap(now2); v.loadSnapBase = base2; v.loadSnapPos = pos; return; }

    const unsigned n = (unsigned)v.axleZ.size();
    auto liftedIn = [&](const std::vector<float>& p, unsigned memIdx, bool rev) {
        unsigned all = rev ? n - 1 - memIdx : memIdx;
        for (unsigned i = 0; i < v.axles.size(); ++i) if (v.axles[i].allIndex == all) return p[i] > 0.5f;
        return false;
    };
    auto fits = [&](const uint8_t* mem, const std::vector<float>& p, bool rev, float& sum) {
        float vals[16]; sum = 0.0f;
        for (unsigned k = 0; k < n; ++k) {
            memcpy(&vals[k], mem + 4 * k, 4);
            if (!std::isfinite(vals[k]) || vals[k] < -100.0f || vals[k] > 5.0e6f) return false;
            sum += std::max(vals[k], 0.0f);
        }
        if (sum < 1000.0f) return false;                   // < ~0.1 t: not a load
        for (unsigned k = 0; k < n; ++k) {
            bool lifted = liftedIn(p, k, rev);
            if (lifted && vals[k] > 0.02f * sum) return false;
            if (!lifted && vals[k] < 0.02f * sum) return false;
        }
        return true;
    };

    uintptr_t best = 0; bool bestRev = false; size_t bestDist = SIZE_MAX;
    for (size_t off = 0; off + 4 * n <= now2.size(); off += 4) {
        for (int rev = 0; rev < 2; ++rev) {
            float s1, s2;
            if (!fits(&v.loadSnap[off], v.loadSnapPos, rev != 0, s1) || !fits(&now2[off], pos, rev != 0, s2)) continue;
            if (std::fabs(s1 - s2) > 0.1f * std::max(s1, s2)) continue;    // total weight is the same
            uintptr_t addr = base2 + off;
            size_t dist = addr > v.axleList ? addr - v.axleList : v.axleList - addr;
            if (dist < bestDist) { best = addr; bestRev = rev != 0; bestDist = dist; }
        }
    }
    if (best) {
        v.loadArray = best;
        v.loadReversed = bestRev;
        Wipe(v.loadSnap);
        LogInfo("[axlelift] %s: axle loads found at %p", v.name, (void*)best);
    } else {
        Wipe(v.loadSnap); v.loadSnap.swap(now2); v.loadSnapPos = pos;   // try again with the next change
        LogDebug("[axlelift] %s: axle loads not found yet", v.name);
    }
    Wipe(now2);
}

// ---------------------------------------------------------------------------
// identical trailers (e.g. a B-double) lifting together: every matching lift list looks the same.
// Probe: nudge one list's first lift axle by 8 % for a few frames and watch which trailer's wheels
// react in telemetry (lift offset / suspension). Always restored. Falls back to "Swap" in the overlay.
// ---------------------------------------------------------------------------
struct ProbeState {
    bool active = false;
    bool verify = false;              // only checking that one vehicle's data is still live
    std::vector<std::pair<uintptr_t, bool>> lists;
    std::vector<unsigned> slots;
    size_t idx = 0;
    int frame = 0;
    uintptr_t addr = 0;
    float orig = 0.0f, nudge = 0.0f;
    float baseOff[VEHICLE_COUNT] = {}, baseDef[VEHICLE_COUNT] = {}, react[VEHICLE_COUNT] = {};
    std::vector<int> owner;           // per list: slot that reacted, -1 = none
    std::vector<float> ownerReact;
};
static ProbeState g_probe;
static bool g_probeWaitDown = false;   // last check was with the axles up and unclear: retry when down
bool ProbeActive() { return g_probe.active; }

static void ProbeSignal(const Vehicle& v, float& off, float& def)
{
    off = def = 0.0f;
    for (unsigned w : v.axles[0].wheels) { off += v.wheels[w].liftOffset; def += v.wheels[w].deflection; }
}

static void ProbeAbort(const char* why)
{
    if (g_probe.addr && g_probe.frame > 0 && g_probe.frame <= 11) WriteFloat(g_probe.addr, g_probe.orig);
    for (unsigned s : g_probe.slots) g_vehicles[s].probeNeeded = false;
    g_probe = ProbeState{};
    LogInfo("[axlelift] trailer check stopped: %s", why);
}

static void ProbeFinish()
{
    ProbeState& p = g_probe;
    if (p.verify) {                   // one vehicle, one list: did its wheel react?
        unsigned s = p.slots[0];
        Vehicle& v = g_vehicles[s];
        v.verifyNeeded = false;
        bool live = !p.owner.empty() && p.owner[0] == (int)s;
        g_probe = ProbeState{};
        if (live) LogDebug("[axlelift] %s: lift data still live", v.name);
        else DataLost(s);
        return;
    }
    std::vector<int> listOf(VEHICLE_COUNT, -1);
    // each list -> the trailer that reacted clearly; one list per trailer
    for (size_t i = 0; i < p.lists.size(); ++i) {
        int s = p.owner[i];
        if (s < 0) continue;
        if (listOf[s] >= 0 && p.ownerReact[listOf[s]] >= p.ownerReact[i]) continue;
        listOf[s] = (int)i;
    }
    // one trailer and one list left over: they belong together
    std::vector<unsigned> openSlots; std::vector<size_t> openLists;
    for (unsigned s : p.slots) if (listOf[s] < 0) openSlots.push_back(s);
    for (size_t i = 0; i < p.lists.size(); ++i) {
        bool used = false;
        for (unsigned s : p.slots) used |= listOf[s] == (int)i;
        if (!used) openLists.push_back(i);
    }
    if (openSlots.size() == 1 && openLists.size() == 1) listOf[openSlots[0]] = (int)openLists[0];

    bool wasUp = false;
    for (auto& l : p.lists) {
        const Vehicle& ref = g_vehicles[p.slots[0]];
        unsigned n = (unsigned)ref.axleZ.size(), ai = ref.axles[0].allIndex;
        float x = 0; ReadMem(l.first + 4 * (l.second ? n - 1 - ai : ai), &x, 4);
        wasUp |= x > 0.5f;
    }
    unsigned solved = 0;
    for (unsigned s : p.slots) {
        Vehicle& v = g_vehicles[s];
        v.probeNeeded = false;
        if (listOf[s] < 0) { v.probeNeeded = wasUp; continue; }   // up: wheels in the air hardly react -> retry when down
        uintptr_t list = p.lists[listOf[s]].first;
        if (v.axleList != list) {      // other trailer's data: take ours, measure loads again
            v.axleList = list;
            v.listReversed = p.lists[listOf[s]].second;
            v.loadArray = 0; Wipe(v.loadSnap); v.loadSnapPos.clear(); v.loadSnapAt = 0.0;
        }
        v.uncertain = false;
        ++solved;
    }
    g_probeWaitDown = solved < p.slots.size() && wasUp;
    if (solved == p.slots.size()) Notify("Trailers told apart automatically");
    else if (g_probeWaitDown) LogInfo("[axlelift] trailer check unclear with the axles up - trying again when they are down");
    else Notify("Could not tell the trailers apart - use 'Swap' in the overlay if the wrong one moves");
    LogInfo("[axlelift] trailer check: %u of %zu trailers assigned", solved, p.slots.size());
    g_probe = ProbeState{};
}

static void ProbeTick()
{
    ProbeState& p = g_probe;
    if (g_blocked) { if (p.active) ProbeAbort("multiplayer"); return; }

    if (!p.active) {
        // re-sent vehicle (cargo loaded, ...): nudge its own list once and see if the wheel reacts
        for (const Vehicle& v : g_vehicles) {
            if (!v.present || !v.found || !v.verifyNeeded || v.probeNeeded) continue;
            if (!v.offsetSeen) { g_vehicles[v.slot].verifyNeeded = false; continue; }   // can't check without wheel heights
            bool still = true;
            for (auto& a : v.axles) {
                float gl = v.wheels[a.wheels[0]].gameLift;
                if (a.controlled || (gl > 0.001f && gl < 0.999f)) still = false;
            }
            if (!still) continue;
            p = ProbeState{};
            p.active = true;
            p.verify = true;
            p.slots.push_back(v.slot);
            p.lists.push_back({ v.axleList, v.listReversed });
            p.owner.assign(1, -1);
            p.ownerReact.assign(1, 0.0f);
            LogDebug("[axlelift] %s: checking the lift data after a change", v.name);
            break;
        }
    }
    if (!p.active) {
        std::vector<unsigned> slots;
        for (const Vehicle& v : g_vehicles) {
            if (!v.present || v.axles.empty()) continue;
            if (!v.found && v.scanDone) return;                      // another trailer is still being found
            if (v.found && v.probeNeeded) slots.push_back(v.slot);
        }
        if (slots.empty()) return;
        for (unsigned s : slots)
            for (auto& a : g_vehicles[s].axles) {
                float gl = g_vehicles[s].wheels[a.wheels[0]].gameLift;
                if (a.controlled || (gl > 0.001f && gl < 0.999f)) return;   // wait until everything is still
                if (g_probeWaitDown && gl > 0.5f) return;                    // retry only with the axles down
            }
        g_probeWaitDown = false;
        p = ProbeState{};
        p.active = true;
        p.slots = slots;
        for (unsigned s : slots)
            for (auto& l : g_vehicles[s].altLists) {
                bool have = false;
                for (auto& x : p.lists) have |= x.first == l.first;
                // lists already settled for a trailer outside this check are not touched
                for (const Vehicle& o : g_vehicles)
                    have |= o.found && !o.probeNeeded && !o.uncertain && o.axleList == l.first;
                if (!have) p.lists.push_back(l);
            }
        p.owner.assign(p.lists.size(), -1);
        p.ownerReact.assign(p.lists.size(), 0.0f);
        LogInfo("[axlelift] telling %zu identical trailers apart (%zu lift lists)", slots.size(), p.lists.size());
    }

    for (unsigned s : p.slots) if (!g_vehicles[s].present || !g_vehicles[s].found) { ProbeAbort("trailer changed"); return; }
    if (p.idx >= p.lists.size()) { ProbeFinish(); return; }

    const Vehicle& ref = g_vehicles[p.slots[0]];
    auto& list = p.lists[p.idx];
    unsigned n = (unsigned)ref.axleZ.size(), ai = ref.axles[0].allIndex;
    if (p.frame == 0) {
        p.addr = list.first + 4 * (list.second ? n - 1 - ai : ai);
        if (!ReadMem(p.addr, &p.orig, 4) || !(p.orig >= -0.01f && p.orig <= 1.01f)) { ++p.idx; return; }
        p.nudge = p.orig > 0.5f ? p.orig - 0.08f : p.orig + 0.08f;
        for (unsigned s : p.slots) { ProbeSignal(g_vehicles[s], p.baseOff[s], p.baseDef[s]); p.react[s] = 0.0f; }
        WriteFloat(p.addr, p.nudge);
        p.frame = 1;
        return;
    }
    if (p.frame <= 10) {
        WriteFloat(p.addr, p.nudge);
        for (unsigned s : p.slots) {
            float off, def; ProbeSignal(g_vehicles[s], off, def);
            p.react[s] = std::max(p.react[s], std::fabs(off - p.baseOff[s]) + std::fabs(def - p.baseDef[s]));
        }
        ++p.frame;
        return;
    }
    if (p.frame == 11) {
        WriteFloat(p.addr, p.orig);
        float best = 0.0f, second = 0.0f; int bestSlot = -1;
        for (unsigned s : p.slots) {
            if (p.react[s] > best) { second = best; best = p.react[s]; bestSlot = (int)s; }
            else if (p.react[s] > second) second = p.react[s];
        }
        if (bestSlot >= 0 && best > 0.002f && best > 3.0f * second) { p.owner[p.idx] = bestSlot; p.ownerReact[p.idx] = best; }
        LogDebug("[axlelift] trailer check list %zu: reaction %.4f (next %.4f) -> %s", p.idx, best, second,
                 p.owner[p.idx] >= 0 ? g_vehicles[p.owner[p.idx]].name : "unclear");
    }
    if (++p.frame > 20) { p.frame = 0; ++p.idx; }   // let it settle before the next list
}

// ---------------------------------------------------------------------------
// per frame
// ---------------------------------------------------------------------------
void ScanTick()
{
    bool scannedThisFrame = false; // at most one full scan per frame
    double now = NowSeconds();
    ProbeTick();

    for (Vehicle& v : g_vehicles) {
        if (!v.present || v.axles.empty()) continue;
        if (v.found) { if (!g_probe.active && !v.probeNeeded) FindLoads(v, now); continue; }

        float L = v.wheels[v.axles[0].wheels[0]].gameLift;
        bool atRest = L < 0.001f || L > 0.999f;
        bool lastAtRest = v.lastL < 0.001f || v.lastL > 0.999f;

        if (v.scanArmed && !v.scanDone && L > 0.1f && L < 0.9f && !scannedThisFrame) {
            double t0 = NowSeconds();
            bool ok = FirstScan(v, L, 0.0005f);
            scannedThisFrame = true;
            v.scanArmed = false;
            if (!ok) { Log(SCS_LOG_TYPE_warning, "[axlelift] %s: too many matches, trying again next time", v.name); v.scanArmed = true; continue; }
            v.scanDone = true; v.lastL = L;
            LogInfo("[axlelift] %s: search at lift %.3f: %zu candidates (%.0f ms)", v.name, L, v.cand.size(), (NowSeconds() - t0) * 1000.0);
            continue;
        }
        if (v.scanDone && !v.cand.empty() && (std::fabs(L - v.lastL) > 0.005f || (atRest && !lastAtRest))) {
            Refine(v, L, atRest ? 0.001f : 0.0005f);
            v.lastL = L;
            if (atRest && v.cand.size() <= 1024) Detect(v, L);
            if (!v.found && v.cand.empty()) { v.scanDone = false; v.scanArmed = true; } // lost it: try again
        }
    }
}

// ---------------------------------------------------------------------------
// game version
// ---------------------------------------------------------------------------
bool CheckGameVersion(char* out, size_t n)
{
    snprintf(out, n, "unknown");
    char exe[MAX_PATH];
    if (!GetModuleFileNameA(nullptr, exe, MAX_PATH)) return false;
    DWORD handle = 0, size = GetFileVersionInfoSizeA(exe, &handle);
    if (!size) return false;
    std::vector<uint8_t> data(size);
    if (!GetFileVersionInfoA(exe, 0, size, data.data())) return false;
    VS_FIXEDFILEINFO* fi = nullptr; UINT len = 0;
    if (!VerQueryValueA(data.data(), "\\", (LPVOID*)&fi, &len) || !fi) return false;
    snprintf(out, n, "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
             HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
    for (const char* t : kTestedVersions) if (!strcmp(t, out)) return true;
    return false;
}

// ---------------------------------------------------------------------------
// research export (diagnostics v2) - written next to the DLL.
// Goal: find a stable path from something static to each trailer's lift data, so setup works
// without the first lift. Follows "who points here" three levels up and records wheel telemetry.
// The plugin's own buffers are excluded (and wiped when freed) so they don't show up as hits.
// ---------------------------------------------------------------------------
static void HexDump(FILE* f, uintptr_t addr, size_t len)
{
    std::vector<uint8_t> b(len);
    for (size_t o = 0; o < len; o += 16) {
        bool ok = ReadMem(addr + o, &b[o], 16);
        fprintf(f, "    %p ", (void*)(addr + o));
        for (size_t k = 0; k < 16; ++k) ok ? fprintf(f, " %02X", b[o + k]) : fprintf(f, " ??");
        fprintf(f, "  ");
        for (size_t k = 0; k < 16; ++k) fputc(ok && b[o + k] >= 32 && b[o + k] < 127 ? b[o + k] : '.', f);
        fprintf(f, "\n");
    }
}

struct DiagNode { uintptr_t addr; int parent; int level; uint64_t value; char what[48]; };

static bool ModuleOf(uintptr_t a, char* name, size_t n, uintptr_t& base)
{
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) || !m) return false;
    char full[MAX_PATH]; GetModuleFileNameA(m, full, MAX_PATH);
    const char* b = strrchr(full, '\\');
    snprintf(name, n, "%s", b ? b + 1 : full);
    base = (uintptr_t)m;
    return true;
}

static void Describe(char* out, size_t n, uint64_t p)
{
    char mod[64]; uintptr_t base;
    if (p > 0x10000 && ModuleOf((uintptr_t)p, mod, sizeof(mod), base)) snprintf(out, n, "%s+0x%llX", mod, (unsigned long long)(p - base));
    else snprintf(out, n, "%p", (void*)p);
}

void WriteDiagnostics()
{
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&WriteDiagnostics, &self);
    char path[MAX_PATH]; GetModuleFileNameA(self, path, MAX_PATH);
    char* slash = strrchr(path, '\\'); if (slash) *slash = 0;
    strncat(path, "\\axlelift_diagnostics.txt", MAX_PATH - strlen(path) - 1);
    FILE* f = fopen(path, "w");
    if (!f) { Notify("Could not write %s", path); return; }
    double t0 = NowSeconds();

    char ver[32]; bool tested = CheckGameVersion(ver, sizeof(ver));
    InExe(0);
    fprintf(f, "Lift Axle Extension %s diagnostics v2\ngame %s (%s)\nexe %p - %p\n\n", AXLELIFT_VERSION, ver,
            tested ? "tested" : "untested", (void*)g_exeBase, (void*)g_exeEnd);

    // ---- vehicles + wheel telemetry
    std::vector<DiagNode> nodes;
    for (const Vehicle& v : g_vehicles) {
        if (!v.present) continue;
        fprintf(f, "== %s (%s): %u wheels, %u axles, found=%d uncertain=%d list=%p rev=%d loads=%p alt=%zu\n", v.name, v.id,
                v.wheelCount, (unsigned)v.axleZ.size(), v.found, v.uncertain, (void*)v.axleList, v.listReversed, (void*)v.loadArray, v.altLists.size());
        for (unsigned i = 0; i < v.wheelCount; ++i) {
            const Wheel& w = v.wheels[i];
            fprintf(f, "   wheel %u liftable=%d z=%.3f lift=%.3f offset=%.5f deflection=%.5f ground=%d\n", i, w.liftable, w.posZ,
                    w.gameLift, w.liftOffset, w.deflection, w.onGround);
        }
        if (!v.found) continue;
        for (auto& l : v.altLists) {
            DiagNode d{ l.first, -1, 0, 0, "" };
            snprintf(d.what, sizeof(d.what), "%s lift list%s", v.name, l.first == v.axleList ? "" : " (other match)");
            nodes.push_back(d);
            fprintf(f, "  %s at %p:\n", d.what, (void*)l.first); HexDump(f, l.first - 0x30, 0x60);
        }
        if (v.loadArray) {
            DiagNode d{ v.loadArray, -1, 0, 0, "" };
            snprintf(d.what, sizeof(d.what), "%s loads", v.name);
            nodes.push_back(d);
            fprintf(f, "  loads at %p:\n", (void*)v.loadArray); HexDump(f, v.loadArray - 0x30, 0x60);
        }

        // where do the wheel telemetry values live? (exact float match near the lift list)
        const size_t W = 1 << 20;
        std::vector<uint8_t> win(2 * W, 0);
        uintptr_t wb = (v.axleList - W) & ~(uintptr_t)0xFFF;
        for (size_t o = 0; o < win.size(); o += 4096) ReadMem(wb + o, &win[o], 4096);
        for (unsigned i = 0; i < v.wheelCount; ++i) {
            const float vals[2] = { v.wheels[i].deflection, v.wheels[i].liftOffset };
            const char* names[2] = { "deflection", "offset" };
            for (int k = 0; k < 2; ++k) {
                if (std::fabs(vals[k]) < 1e-5f) continue;
                int found = 0;
                fprintf(f, "   wheel %u %s %.6f found at list", i, names[k], vals[k]);
                for (size_t o = 0; o + 4 <= win.size() && found < 8; o += 4)
                    if (!memcmp(&win[o], &vals[k], 4)) { fprintf(f, " %+lld", (long long)(wb + o) - (long long)v.axleList); ++found; }
                fprintf(f, found ? "\n" : " (not within 1 MB)\n");
            }
        }
        Wipe(win);
        fprintf(f, "\n");
    }

    // ---- pointer chain: level 1 = who points at the data, level 2/3 = who points at those objects
    std::vector<uint64_t> buf(1 << 17);        // 1 MB, never resized (excluded from the search itself)
    auto ownBuffer = [&](uintptr_t a) {
        if (a >= (uintptr_t)buf.data() && a < (uintptr_t)(buf.data() + buf.size())) return true;
        if (!nodes.empty() && a >= (uintptr_t)nodes.data() && a < (uintptr_t)(nodes.data() + nodes.capacity())) return true;
        for (const Vehicle& v : g_vehicles) {
            auto in = [&](const void* d, size_t bytes) { return d && a >= (uintptr_t)d && a < (uintptr_t)d + bytes; };
            if (in(v.cand.data(), v.cand.capacity() * 8) || in(v.wheelTargets.data(), v.wheelTargets.capacity() * sizeof(WheelTarget)) ||
                in(v.loadSnap.data(), v.loadSnap.capacity()) || in(v.altLists.data(), v.altLists.capacity() * sizeof(v.altLists[0]))) return true;
        }
        return false;
    };
    nodes.reserve(4096);                       // keep the vector from moving while we search
    const size_t levelCap = 300;
    for (int level = 1; level <= 3; ++level) {
        // ranges: a pointer "to" a node may point a bit before it (start of the containing object)
        struct R { uint64_t lo, hi; int node; };
        std::vector<R> ranges;
        for (int i = 0; i < (int)nodes.size(); ++i)
            if (nodes[i].level == level - 1) {
                uint64_t back = level == 1 ? 0x40 : 0x400;
                ranges.push_back({ nodes[i].addr - back, nodes[i].addr, i });
            }
        if (ranges.empty()) break;
        std::sort(ranges.begin(), ranges.end(), [](const R& x, const R& y) { return x.hi < y.hi; });
        size_t before = nodes.size(), added = 0;
        ForEachResidentRun([&](uintptr_t addr, size_t n) {
            for (size_t done = 0; done < n; done += buf.size() * 8) {
                size_t chunk = std::min(n - done, buf.size() * 8);
                if (!ReadMem(addr + done, buf.data(), chunk)) continue;
                for (size_t i = 0; i < chunk / 8; ++i) {
                    uint64_t p = buf[i];
                    auto it = std::lower_bound(ranges.begin(), ranges.end(), p, [](const R& r, uint64_t x) { return r.hi < x; });
                    for (; it != ranges.end() && it->lo <= p; ++it) {
                        uintptr_t at = addr + done + i * 8;
                        const DiagNode& target = nodes[it->node];
                        if (at >= target.addr - 0x40 && at < target.addr + 0x40) continue;   // allocator bookkeeping of the target itself
                        if (ownBuffer(at)) continue;
                        DiagNode d{ at, it->node, level, p, "" };
                        nodes.push_back(d);
                        if (++added >= levelCap) return false;
                        break;
                    }
                }
            }
            return true;
        });
        fprintf(f, "---- level %d: %zu pointers%s\n", level, added, added >= levelCap ? " (limit reached)" : "");
        for (size_t i = before; i < nodes.size(); ++i) {
            const DiagNode& d = nodes[i];
            const DiagNode& t = nodes[d.parent];
            char where[96], vt[96]; Describe(where, sizeof(where), d.addr);
            uint64_t q = 0; ReadMem((d.addr & ~(uintptr_t)7) - 8, &q, 8); Describe(vt, sizeof(vt), q);
            // walk back to the root target for the label
            int r = d.parent; while (nodes[r].parent >= 0) r = nodes[r].parent;
            fprintf(f, "[L%d #%zu] at %s -> %p (= node #%d %+lld)  chain to: %s   qword before: %s\n", level, i, where, (void*)d.value,
                    d.parent, (long long)d.value - (long long)t.addr, nodes[r].what, vt);
            HexDump(f, (d.addr - 0x40) & ~(uintptr_t)0xF, 0x90);
        }
        fprintf(f, "\n");
    }
    fprintf(f, "done in %.1f s\n", NowSeconds() - t0);
    fclose(f);
    Wipe(buf);
    Wipe(nodes);
    Notify("Diagnostics written to %s", path);
}
