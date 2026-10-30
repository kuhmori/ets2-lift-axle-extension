// memory: find each vehicle's lift data and steering values, tell identical trailers apart, game version, research export
//
// Layout found on ETS2 1.61.1.9 (checked structurally at run time, never trusted blindly):
//   axle list    one float per axle (0 = down, 1 = up), front to back; drives the physics
//   wheel object [obj+0x00] vtable inside eurotrucks2.exe, [obj+0x20] u32 wheel index,
//                lift float at obj+0x80 and obj+0x150 (visual / suspension)
#include "common.h"

#include <emmintrin.h>

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

static const size_t MAX_STEER_CANDIDATES = 4000000;

// ---------------------------------------------------------------------------
// background search: reads all of the game's memory on a worker thread so the game keeps running.
// The value we look for keeps changing while the search runs, so the game thread feeds the
// latest telemetry values in every frame and each 1 MB chunk is compared against a narrow band
// around the value at that moment (last value, previous value and where it is heading).
// The game thread then confirms the matches over the next frames as before.
// ---------------------------------------------------------------------------
struct ScanSeries {
    uint8_t axle, unit;
    float prev, latest, tol;
    float minAbs, maxAbs;   // value at its rest position (0 / straight, or fully up) is everywhere in memory -> not searched

    bool valid;
};
struct ScanHit { uintptr_t addr; uint8_t axle, unit; };

struct ScanJob {
    std::thread th;
    std::atomic<int> state{0};          // 0 idle, 1 running, 2 finished, 3 too many matches
    std::atomic<bool> abort{false};
    bool steer = false;
    unsigned slot = 0, gen = 0;
    double startedAt = 0.0;
    size_t skipped = 0, scanned = 0;    // bytes read while the value was at rest (not searched)
    std::mutex m;
    std::vector<ScanSeries> series;     // game thread updates prev / latest
    std::vector<ScanHit> hits;
    // lift search only: layout, to still recognise the data once the axles are fully up
    std::vector<uint8_t> liftPattern;   // per axle (front to back): 1 = lift axle, 0 = not
    uint32_t liftableWheels = 0;        // bit per wheel index
    unsigned wheelCount = 0;
};
static ScanJob g_job;

static bool JobIdle() { return g_job.state == 0; }

static void ScanWorker()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    struct Band { float lo, hi; uint8_t axle, unit; };
    std::vector<Band> bands;
    std::vector<uint8_t> buf(1 << 20);
    const size_t cap = g_job.steer ? MAX_STEER_CANDIDATES : MAX_CANDIDATES;
    bool overflow = false;

    ForEachResidentRun([&](uintptr_t addr, size_t n) {
        if (g_job.abort) return false;
        // bands for this chunk from the newest values
        bands.clear();
        bool restUp = false;                 // lift search: axles stopped fully up
        {
            std::lock_guard<std::mutex> lock(g_job.m);
            for (const ScanSeries& sr : g_job.series) {
                if (!g_job.steer && sr.valid && sr.latest > sr.maxAbs) restUp = true;
                if (!sr.valid || std::fabs(sr.latest) < sr.minAbs || std::fabs(sr.latest) > sr.maxAbs) continue;
                float next = sr.latest + 2.0f * (sr.latest - sr.prev);
                float lo = std::min(std::min(sr.prev, sr.latest), next) - sr.tol;
                float hi = std::max(std::max(sr.prev, sr.latest), next) + sr.tol;
                bands.push_back({ lo, hi, sr.axle, sr.unit });
            }
        }
        if (bands.empty() && restUp && !g_job.liftPattern.empty()) {
            // 1.0 is far too common to collect on its own: only take it where it sits in the
            // known lift layout (axle list, either order) or in a lift wheel object
            if (n > buf.size()) buf.resize(n);
            if (!ReadMem(addr, buf.data(), n)) return true;
            g_job.scanned += n;
            const auto& pat = g_job.liftPattern;
            const unsigned cnt = (unsigned)pat.size();
            auto at = [&](long long o, float& f) { if (o < 0 || o + 4 > (long long)n) return false; memcpy(&f, &buf[(size_t)o], 4); return true; };
            const __m128i one = _mm_set1_epi32(0x3F800000);
            for (size_t off = 0; off + 16 <= n; off += 16) {
                int m = _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(_mm_loadu_si128((const __m128i*)&buf[off]), one)));
                if (!m) continue;
                for (int k = 0; k < 4; ++k) {
                    if (!(m & (1 << k))) continue;
                    long long c = (long long)(off + 4 * k);
                    // axle list: c is the first lift axle (front-to-back or back-to-front)
                    for (int rev = 0; rev < 2; ++rev) {
                        // memory slot q holds axle q (rev = 0) or axle cnt-1-q (rev = 1); fi = first lift slot
                        unsigned fi = 0;
                        for (unsigned q = 0; q < cnt; ++q) if (rev ? pat[cnt - 1 - q] : pat[q]) { fi = q; break; }
                        long long base = c - 4LL * fi;
                        bool ok = true;
                        for (unsigned q = 0; q < cnt && ok; ++q) {
                            bool lift = rev ? pat[cnt - 1 - q] != 0 : pat[q] != 0;
                            float f; ok = at(base + 4LL * q, f) && f == (lift ? 1.0f : 0.0f);
                        }
                        if (!ok) continue;
                        for (unsigned q = 0; q < cnt; ++q)
                            if (rev ? pat[cnt - 1 - q] : pat[q]) g_job.hits.push_back({ addr + (size_t)(base + 4LL * q), 0, 0 });
                    }
                    // lift wheel object: vtable in the exe at -0x80, wheel index at -0x60, second copy at +0xD0
                    uint64_t vt = 0; uint32_t idx = 0; float second = 0.0f;
                    if (c >= 0x80 && (size_t)c + 0xD4 <= n) {
                        memcpy(&vt, &buf[(size_t)c - 0x80], 8); memcpy(&idx, &buf[(size_t)c - 0x60], 4); memcpy(&second, &buf[(size_t)c + 0xD0], 4);
                        if (second == 1.0f && idx < g_job.wheelCount && (g_job.liftableWheels >> idx & 1) && InExe(vt))
                            g_job.hits.push_back({ addr + (size_t)c, 0, 0 });
                    }
                }
                if (g_job.hits.size() >= cap) { overflow = true; return false; }
            }
            return true;
        }
        if (bands.empty()) { g_job.skipped += n; return true; }
        float absLo = 1e30f, absHi = 0.0f;
        for (const Band& b : bands) {
            absLo = std::min(absLo, (b.lo <= 0.0f && b.hi >= 0.0f) ? 0.0f : std::min(std::fabs(b.lo), std::fabs(b.hi)));
            absHi = std::max(absHi, std::max(std::fabs(b.lo), std::fabs(b.hi)));
        }
        uint32_t bitsLo, bitsHi;
        memcpy(&bitsLo, &absLo, 4); memcpy(&bitsHi, &absHi, 4);

        if (n > buf.size()) buf.resize(n);
        if (!ReadMem(addr, buf.data(), n)) return true;
        g_job.scanned += n;
        auto check = [&](size_t off) {
            float f; memcpy(&f, &buf[off], 4);
            for (const Band& b : bands)
                if (f >= b.lo && f <= b.hi) { g_job.hits.push_back({ addr + off, b.axle, b.unit }); return; }
        };
        // 4 floats at a time: |f| (as integer bits) inside the band range?
        const __m128i absMask = _mm_set1_epi32(0x7FFFFFFF);
        const __m128i vLo = _mm_set1_epi32((int)bitsLo - 1), vHi = _mm_set1_epi32((int)bitsHi + 1);
        size_t off = 0;
        for (; off + 16 <= n; off += 16) {
            __m128i x = _mm_and_si128(_mm_loadu_si128((const __m128i*)&buf[off]), absMask);
            int m = _mm_movemask_ps(_mm_castsi128_ps(_mm_and_si128(_mm_cmpgt_epi32(x, vLo), _mm_cmplt_epi32(x, vHi))));
            if (!m) continue;
            for (int k = 0; k < 4; ++k) if (m & (1 << k)) check(off + 4 * k);
            if (g_job.hits.size() >= cap) { overflow = true; return false; }
        }
        for (; off + 4 <= n; off += 4) check(off);
        return true;
    });
    Wipe(buf);
    if (overflow) Wipe(g_job.hits);
    g_job.state = overflow ? 3 : 2;
}

// game thread: newest values for the running search
static void FeedJob(float (*value)(const Vehicle&, const ScanSeries&))
{
    if (g_job.state != 1) return;
    const Vehicle& v = g_vehicles[g_job.slot];
    std::lock_guard<std::mutex> lock(g_job.m);
    for (ScanSeries& sr : g_job.series) {
        float x = value(v, sr);
        if (!std::isfinite(x)) { sr.valid = false; continue; }
        if (!sr.valid) { sr.prev = sr.latest = x; sr.valid = true; continue; }
        sr.prev = sr.latest; sr.latest = x;
    }
}

static void StartJob(bool steer, unsigned slot, unsigned gen, std::vector<ScanSeries> series)
{
    if (g_job.th.joinable()) g_job.th.join();
    InExe(0);                                   // module range known before the worker uses it
    g_job.liftPattern.clear(); g_job.liftableWheels = 0; g_job.wheelCount = 0;
    if (!steer) {
        const Vehicle& v = g_vehicles[slot];
        g_job.liftPattern.assign(v.axleZ.size(), 0);
        for (const Axle& a : v.axles) if (a.allIndex < v.axleZ.size()) g_job.liftPattern[a.allIndex] = 1;
        for (unsigned w = 0; w < v.wheelCount && w < 32; ++w) if (v.wheels[w].liftable) g_job.liftableWheels |= 1u << w;
        g_job.wheelCount = v.wheelCount;
    }
    Wipe(g_job.hits);
    g_job.series = std::move(series);
    g_job.steer = steer; g_job.slot = slot; g_job.gen = gen;
    g_job.skipped = g_job.scanned = 0;
    g_job.startedAt = NowSeconds();
    g_job.abort = false;
    g_job.state = 1;
    g_job.th = std::thread(ScanWorker);
}

void ScanShutdown()
{
    g_job.abort = true;
    if (g_job.th.joinable()) g_job.th.join();
    Wipe(g_job.hits);
    g_job.state = 0;
}

static float LiftValue(const Vehicle& v, const ScanSeries&) { return v.axles.empty() ? NAN : v.wheels[v.axles[0].wheels[0]].gameLift; }

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
    v.scanArmed = v.scanDone = v.scanRunning = false;
    ++v.liftGen;
    v.found = v.uncertain = false;
    v.axleList = 0; v.listReversed = false;
    Wipe(v.wheelTargets);
    Wipe(v.altLists);
    v.probeNeeded = false;
    v.verifyNeeded = false;
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
    v.detectFails = 0;
    Wipe(v.cand);
    LogInfo("[axlelift] %s: ready (lift data %p%s, %zu wheel objects%s)", v.name, (void*)v.axleList,
            v.listReversed ? ", rear first" : "", v.wheelTargets.size(), v.uncertain ? ", several matches" : "");
    Notify("%s: lift axles ready", v.name);
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
        if (v.axleList != list) {      // other trailer's data: take ours
            v.axleList = list;
            v.listReversed = p.lists[listOf[s]].second;
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
            if (!v.found && (v.scanDone || v.scanRunning)) return;   // another trailer is still being found
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
// steering: find where each lockable steered axle keeps its steering angle.
// Telemetry tells us the angle (rotations); memory may hold it as rotations or radians, either
// sign. First search: every float matching a steered wheel's angle. Then follow the angle while
// the axle steers; only values that track it through several different angles are kept.
// ---------------------------------------------------------------------------
static const float  TWO_PI = 6.28318531f;

float SteerExpected(unsigned unit, float rot)
{
    float v = unit >= 2 ? rot * TWO_PI : rot;
    return (unit & 1) ? -v : v;
}
static float SteerTol(unsigned unit) { return unit >= 2 ? 1.2e-4f : 2.0e-5f; }

// does this memory value match the current angle of the target's axle (any of its wheels)?
bool SteerMatches(const Vehicle& v, const SteerTarget& t, float value)
{
    if (t.axle >= v.steerAxles.size()) return false;
    for (unsigned w : v.steerAxles[t.axle].wheels)
        if (std::fabs(value - SteerExpected(t.unit, v.wheels[w].steering)) < SteerTol(t.unit)) return true;
    return false;
}

bool SteerAxleReady(const Vehicle& v, unsigned axle)
{
    for (const SteerTarget& t : v.steerCand) if (t.axle == axle) return true;
    return false;
}

void ResetSteer(Vehicle& v)
{
    Wipe(v.steerCand);
    Wipe(v.steerPend);
    v.steerPendMask = 0; v.steerPendRefines = 0;
    v.steerSearching = v.steerFound = false;
    v.steerRetryAt = 0.0;
    ++v.steerGen;
    for (float& m : v.steerMax) m = 0.0f;
    for (auto& a : v.steerAxles) { a.locking = a.holding = false; a.blend = 1.0f; a.noEffectWarned = false; }
}

// series index -> (wheel, unit): series.axle holds the WHEEL here, the axle is looked up when done
static float SteerValue(const Vehicle& v, const ScanSeries& sr)
{
    return sr.axle < v.wheelCount ? SteerExpected(sr.unit, v.wheels[sr.axle].steering) : NAN;
}

static void StartSteerJob(Vehicle& v, uint32_t mask)
{
    std::vector<ScanSeries> series;
    for (unsigned i = 0; i < v.steerAxles.size(); ++i) {
        if (!(mask & (1u << i))) continue;
        for (unsigned w : v.steerAxles[i].wheels)
            for (uint8_t u = 0; u < 4; ++u) {
                float x = SteerExpected(u, v.wheels[w].steering);
                float scale = u >= 2 ? TWO_PI : 1.0f;
                series.push_back({ (uint8_t)w, u, x, x, SteerTol(u), 0.004f * scale, 1e3f, true });
            }
    }
    v.steerSearching = true;
    v.steerPendMask = mask;
    StartJob(true, v.slot, v.steerGen, std::move(series));
    LogDebug("[axlelift] %s: steering search started in the background", v.name);
}

// keep candidates that still match; a value may move to another axle the search was for
static void SteerRefinePend(Vehicle& v)
{
    std::vector<SteerTarget> keep;
    keep.reserve(v.steerPend.size() / 4 + 16);
    uint8_t page[4096];
    uintptr_t cur = 0; bool ok = false;
    for (SteerTarget t : v.steerPend) {
        uintptr_t pg = t.addr & ~(uintptr_t)0xFFF;
        if (pg != cur || !cur) { cur = pg; ok = ReadMem(pg, page, sizeof(page)); }
        if (!ok) continue;
        float f; memcpy(&f, &page[t.addr - pg], 4);
        if (SteerMatches(v, t, f)) { keep.push_back(t); continue; }
        for (uint8_t a = 0; a < v.steerAxles.size(); ++a) {
            if (a == t.axle || !(v.steerPendMask & (1u << a))) continue;
            SteerTarget o = t; o.axle = a;
            if (SteerMatches(v, o, f)) { keep.push_back(o); break; }
        }
    }
    v.steerPend.swap(keep);
    v.steerPend.shrink_to_fit();
    Wipe(keep);
}

static void SteerScanTick()
{
    double now = NowSeconds();
    for (Vehicle& v : g_vehicles) {
        if (!v.present || v.steerAxles.empty()) continue;

        float moved = 0.0f;
        for (unsigned i = 0; i < v.steerAxles.size(); ++i)
            for (unsigned w : v.steerAxles[i].wheels) {
                moved = std::max(moved, std::fabs(v.wheels[w].steering - v.steerLast[w]));
                v.steerMax[i] = std::max(v.steerMax[i], std::fabs(v.wheels[w].steering));
            }
        auto remember = [&]() { for (unsigned w = 0; w < v.wheelCount; ++w) v.steerLast[w] = v.wheels[w].steering; };

        // confirmed values: still the right memory? (not while we write them ourselves, nor right after)
        bool locking = false;
        for (auto& a : v.steerAxles) locking |= a.locking;
        if (!v.steerCand.empty() && !locking && now - v.steerUnlockedAt >= 0.5) {
            for (size_t i = 0; i < v.steerCand.size();) {
                SteerTarget& t = v.steerCand[i];
                float f;
                if (ReadMem(t.addr, &f, 4) && SteerMatches(v, t, f)) t.miss = 0;
                else if (++t.miss > 60) {
                    unsigned ax = t.axle;
                    v.steerCand.erase(v.steerCand.begin() + i);
                    if (!SteerAxleReady(v, ax))
                        Log(SCS_LOG_TYPE_warning, "[axlelift] %s steered axle %u: steering memory no longer valid - searching again when it steers", v.name, ax + 1);
                    continue;
                }
                ++i;
            }
            v.steerFound = !v.steerCand.empty();
        }

        // search results being confirmed: follow the angle through several different values
        if (!v.steerPend.empty() && moved >= 0.002f) {
            SteerRefinePend(v);
            remember();
            ++v.steerPendRefines;
            if (v.steerPend.empty()) {
                LogDebug("[axlelift] %s: steering search found nothing that follows the wheels - trying again later", v.name);
                v.steerRetryAt = now + 5.0;
            } else if (v.steerPendRefines >= 6 && v.steerPend.size() <= 256) {
                uint32_t got = 0;
                for (const SteerTarget& t : v.steerPend) got |= 1u << t.axle;
                for (const SteerTarget& t : v.steerPend)
                    if (!SteerAxleReady(v, t.axle) || (got & (1u << t.axle))) v.steerCand.push_back(t);
                Wipe(v.steerPend);
                v.steerFound = true;
                v.steerRetryAt = now + 10.0;    // axles that didn't show up: next search not right away
                LogInfo("[axlelift] %s: steering lock ready (%zu values)", v.name, v.steerCand.size());
                for (auto& t : v.steerCand) LogDebug("[axlelift]   steering %p axle %u unit %u", (void*)t.addr, t.axle + 1, t.unit);
                Notify("%s: steering lock ready", v.name);
            }
        }
        if (!v.steerPend.empty() && now - v.steerPendSince > 120.0) {   // never confirmed: start over later
            Wipe(v.steerPend);
            v.steerRetryAt = now + 5.0;
        }

        // start a search for axles that steer but have no values yet (one search at a time, in the background)
        if (v.steerSearching || !v.steerPend.empty() || now < v.steerRetryAt || !JobIdle()) continue;
        uint32_t mask = 0;
        for (unsigned i = 0; i < v.steerAxles.size(); ++i) {
            if (SteerAxleReady(v, i)) continue;
            float cur = 0.0f;
            for (unsigned w : v.steerAxles[i].wheels) cur = std::max(cur, std::fabs(v.wheels[w].steering));
            if (cur > 0.01f) mask |= 1u << i;
        }
        if (mask) { remember(); StartSteerJob(v, mask); }
    }
}

// a background search finished: hand the result to its vehicle
static void FinishJob()
{
    if (g_job.state != 2 && g_job.state != 3) return;
    if (g_job.th.joinable()) g_job.th.join();
    Vehicle& v = g_vehicles[g_job.slot];
    double ms = (NowSeconds() - g_job.startedAt) * 1000.0;
    bool tooMany = g_job.state == 3;
    unsigned skippedPct = g_job.scanned + g_job.skipped ? (unsigned)(100 * g_job.skipped / (g_job.scanned + g_job.skipped)) : 0;
    if (g_job.steer) {
        v.steerSearching = false;
        if (g_job.gen != v.steerGen || !v.present) { /* vehicle changed meanwhile */ }
        else if (tooMany || g_job.hits.empty()) {
            LogDebug("[axlelift] %s: steering search %s - trying again", v.name, tooMany ? "had too many matches" : "found nothing");
            v.steerRetryAt = NowSeconds() + 5.0;
        } else {
            Wipe(v.steerPend);
            v.steerPend.reserve(g_job.hits.size());
            for (const ScanHit& h : g_job.hits) {
                int ax = h.axle < v.wheelCount ? v.wheels[h.axle].steerAxle : -1;   // series held the wheel
                if (ax >= 0) v.steerPend.push_back({ h.addr, (uint8_t)ax, h.unit, 0, 0.0f, 0.0f });
            }
            v.steerPendRefines = 0;
            v.steerPendSince = NowSeconds();
            LogInfo("[axlelift] %s: steering search: %zu candidates (%.0f ms in the background%s)", v.name, v.steerPend.size(), ms,
                    skippedPct ? ", part searched while straight" : "");
        }
    } else {
        v.scanRunning = false;
        if (g_job.gen != v.liftGen || !v.present) { /* vehicle changed meanwhile */ }
        else if (tooMany) {
            Log(SCS_LOG_TYPE_warning, "[axlelift] %s: too many matches, trying again next time", v.name);
            v.scanArmed = true;
        } else {
            Wipe(v.cand);
            v.cand.reserve(g_job.hits.size());
            for (const ScanHit& h : g_job.hits) v.cand.push_back(h.addr);
            std::sort(v.cand.begin(), v.cand.end());
            v.scanDone = true;
            v.lastL = -1.0f;                   // confirm against the current value right away
            LogInfo("[axlelift] %s: search: %zu candidates (%.0f ms in the background%s)", v.name, v.cand.size(), ms,
                    skippedPct ? ", part searched after the axles stopped" : "");
            if (v.cand.empty()) { v.scanDone = false; v.scanArmed = true; }
        }
    }
    Wipe(g_job.hits);
    g_job.state = 0;
}

// ---------------------------------------------------------------------------
// per frame
// ---------------------------------------------------------------------------
void ScanTick()
{
    // background search: give it the newest values, pick up the result when it's done
    if (g_job.state == 1) FeedJob(g_job.steer ? SteerValue : LiftValue);
    FinishJob();

    ProbeTick();
    SteerScanTick();

    for (Vehicle& v : g_vehicles) {
        if (!v.present || v.axles.empty()) continue;
        if (v.found) continue;

        float L = v.wheels[v.axles[0].wheels[0]].gameLift;
        bool atRest = L < 0.001f || L > 0.999f;
        bool lastAtRest = v.lastL < 0.001f || v.lastL > 0.999f;

        // the axles started moving with the game key: search in the background while they move
        if (v.scanArmed && !v.scanDone && !v.scanRunning && L > 0.02f && L < 0.98f && JobIdle()) {
            v.scanArmed = false;
            v.scanRunning = true;
            // values right at the rest positions (0 / 1) are everywhere in memory: only searched while moving
            std::vector<ScanSeries> series{ { 0, 0, L, L, 0.0005f, 0.02f, 0.98f, true } };
            StartJob(false, v.slot, v.liftGen, std::move(series));
            LogDebug("[axlelift] %s: lift search started in the background (lift %.3f)", v.name, L);
            continue;
        }
        if (v.scanDone && !v.cand.empty() && (std::fabs(L - v.lastL) > 0.005f || (atRest && !lastAtRest))) {
            Refine(v, L, atRest ? 0.001f : 0.0005f);
            v.lastL = L;
            if (atRest && v.cand.size() <= 1024) {
                Detect(v, L);
                if (!v.found) {                  // the search missed the lift data: search again on the next lift
                    Wipe(v.cand);
                    v.scanDone = false; v.scanArmed = true;
                    Notify("%s: lift data not found yet - lift or lower the axles once more with the game key", v.name);
                }
            }
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
        fprintf(f, "== %s (%s): %u wheels, %u axles, found=%d uncertain=%d list=%p rev=%d alt=%zu\n", v.name, v.id,
                v.wheelCount, (unsigned)v.axleZ.size(), v.found, v.uncertain, (void*)v.axleList, v.listReversed, v.altLists.size());
        for (unsigned i = 0; i < v.wheelCount; ++i) {
            const Wheel& w = v.wheels[i];
            fprintf(f, "   wheel %u liftable=%d z=%.3f lift=%.3f offset=%.5f deflection=%.5f ground=%d\n", i, w.liftable, w.posZ,
                    w.gameLift, w.liftOffset, w.deflection, w.onGround);
        }
        for (unsigned i = 0; i < v.wheelCount; ++i)
            if (v.wheels[i].steerable) fprintf(f, "   wheel %u steerable, steering %.5f rot, steer axle %d\n", i, v.wheels[i].steering, v.wheels[i].steerAxle);
        fprintf(f, "  steering: searching=%d found=%d values=%zu pending=%zu (refines %u, axles 0x%X)\n", v.steerSearching, v.steerFound,
                v.steerCand.size(), v.steerPend.size(), v.steerPendRefines, v.steerPendMask);
        if (v.steerFound)
            for (auto& t : v.steerCand) {
                fprintf(f, "  steering value %p axle %u unit %u:\n", (void*)t.addr, t.axle + 1, t.unit);
                HexDump(f, (t.addr - 0x40) & ~(uintptr_t)0xF, 0x90);
            }
        if (!v.found) continue;
        for (auto& l : v.altLists) {
            DiagNode d{ l.first, -1, 0, 0, "" };
            snprintf(d.what, sizeof(d.what), "%s lift list%s", v.name, l.first == v.axleList ? "" : " (other match)");
            nodes.push_back(d);
            fprintf(f, "  %s at %p:\n", d.what, (void*)l.first); HexDump(f, l.first - 0x30, 0x60);
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
            if (in(v.cand.data(), v.cand.capacity() * 8) || in(v.steerCand.data(), v.steerCand.capacity() * sizeof(SteerTarget)) ||
                in(v.steerPend.data(), v.steerPend.capacity() * sizeof(SteerTarget)) || in(v.wheelTargets.data(), v.wheelTargets.capacity() * sizeof(WheelTarget)) ||
                in(v.altLists.data(), v.altLists.capacity() * sizeof(v.altLists[0]))) return true;
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
