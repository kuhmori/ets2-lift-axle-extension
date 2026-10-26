// settings + keybinds, stored in axlelift.ini next to the DLL (bin\win_x64\plugins)
#include "common.h"

Settings g_settings;

static const char* kSlotKeys[VEHICLE_COUNT] = { "truck", "trailer1", "trailer2", "trailer3", "trailer4", "trailer5",
                                                "trailer6", "trailer7", "trailer8", "trailer9", "trailer10" };
const char* VehicleSlotKey(unsigned slot) { return slot < VEHICLE_COUNT ? kSlotKeys[slot] : "?"; }

// ---------------------------------------------------------------------------
// key names
// ---------------------------------------------------------------------------
static void KeyName(int vk, char* out, size_t n)
{
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) snprintf(out, n, "%c", vk);
    else if (vk >= VK_F1 && vk <= VK_F24) snprintf(out, n, "F%d", vk - VK_F1 + 1);
    else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) snprintf(out, n, "Num%d", vk - VK_NUMPAD0);
    else switch (vk) {
        case VK_SPACE: snprintf(out, n, "Space"); break;
        case VK_TAB: snprintf(out, n, "Tab"); break;
        case VK_RETURN: snprintf(out, n, "Enter"); break;
        case VK_INSERT: snprintf(out, n, "Insert"); break;
        case VK_DELETE: snprintf(out, n, "Delete"); break;
        case VK_HOME: snprintf(out, n, "Home"); break;
        case VK_END: snprintf(out, n, "End"); break;
        case VK_PRIOR: snprintf(out, n, "PageUp"); break;
        case VK_NEXT: snprintf(out, n, "PageDown"); break;
        case VK_UP: snprintf(out, n, "Up"); break;
        case VK_DOWN: snprintf(out, n, "Down"); break;
        case VK_LEFT: snprintf(out, n, "Left"); break;
        case VK_RIGHT: snprintf(out, n, "Right"); break;
        case VK_MULTIPLY: snprintf(out, n, "Num*"); break;
        case VK_ADD: snprintf(out, n, "Num+"); break;
        case VK_SUBTRACT: snprintf(out, n, "Num-"); break;
        case VK_DIVIDE: snprintf(out, n, "Num/"); break;
        case VK_DECIMAL: snprintf(out, n, "Num."); break;
        case VK_OEM_COMMA: snprintf(out, n, "Comma"); break;
        case VK_OEM_PERIOD: snprintf(out, n, "Period"); break;
        case VK_OEM_MINUS: snprintf(out, n, "Minus"); break;
        case VK_OEM_PLUS: snprintf(out, n, "Plus"); break;
        default: snprintf(out, n, "Key%02X", vk); break;
    }
}

static int KeyFromName(const char* s)
{
    if (s[0] && !s[1]) {
        char c = s[0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')) return c;
    }
    for (int vk = 1; vk < 256; ++vk) {
        char name[16]; KeyName(vk, name, sizeof(name));
        if (_stricmp(name, s) == 0) return vk;
    }
    return 0;
}

void KeyBindText(const KeyBind& b, char* out, size_t n)
{
    if (!b.valid()) { snprintf(out, n, "-"); return; }
    char key[16]; KeyName(b.vk, key, sizeof(key));
    snprintf(out, n, "%s%s%s%s", b.ctrl ? "Ctrl+" : "", b.alt ? "Alt+" : "", b.shift ? "Shift+" : "", key);
}

void JoyBindText(const JoyBind& b, char* out, size_t n)
{
    if (!b.valid()) { snprintf(out, n, "-"); return; }
    char shortName[20]; snprintf(shortName, sizeof(shortName), "%s", b.name[0] ? b.name : "Controller");
    snprintf(out, n, "%s Btn %d", shortName, b.button + 1);
}

static KeyBind ParseKey(const char* s)
{
    KeyBind b;
    char buf[64]; snprintf(buf, sizeof(buf), "%s", s);
    char* tok = buf;
    while (char* plus = strchr(tok, '+')) {
        if (plus == tok) break; // "Num+" style key names end with '+'
        *plus = 0;
        if (_stricmp(tok, "Ctrl") == 0) b.ctrl = true;
        else if (_stricmp(tok, "Alt") == 0) b.alt = true;
        else if (_stricmp(tok, "Shift") == 0) b.shift = true;
        else { *plus = '+'; break; }
        tok = plus + 1;
    }
    b.vk = (!tok[0] || strcmp(tok, "-") == 0 || _stricmp(tok, "none") == 0) ? 0 : KeyFromName(tok);
    return b;
}

// joystick: JoyBtn:{GUID}:<button>:<name>
static void GuidText(const GUID& g, char* out, size_t n)
{
    snprintf(out, n, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", (unsigned long)g.Data1, g.Data2, g.Data3,
             g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

static bool ParseGuid(const char* s, GUID& g)
{
    unsigned long d1; unsigned d2, d3, b[8];
    if (sscanf(s, "{%8lx-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x}", &d1, &d2, &d3, &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]) != 11)
        return false;
    g.Data1 = d1; g.Data2 = (unsigned short)d2; g.Data3 = (unsigned short)d3;
    for (int i = 0; i < 8; ++i) g.Data4[i] = (unsigned char)b[i];
    return true;
}

static void JoySave(const JoyBind& b, char* out, size_t n)
{
    if (!b.valid()) { snprintf(out, n, "-"); return; }
    char g[48]; GuidText(b.device, g, sizeof(g));
    snprintf(out, n, "JoyBtn:%s:%d:%s", g, b.button, b.name);
}

static JoyBind ParseJoy(const char* s)
{
    JoyBind b;
    if (strncmp(s, "JoyBtn:", 7) != 0) return b;
    const char* g = s + 7;
    const char* c1 = strchr(g, ':'); if (!c1) return b;
    char gs[48] = ""; snprintf(gs, sizeof(gs), "%.*s", (int)(c1 - g), g);
    GUID guid; if (!ParseGuid(gs, guid)) return b;
    int btn = atoi(c1 + 1);
    const char* c2 = strchr(c1 + 1, ':');
    b.device = guid; b.button = btn;
    snprintf(b.name, sizeof(b.name), "%s", c2 ? c2 + 1 : "");
    return b;
}

// ---------------------------------------------------------------------------
// file locations: next to the DLL; Documents only as fallback / for migration
// ---------------------------------------------------------------------------
static bool PluginIniPath(char* out, size_t n)
{
    HMODULE self = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)&PluginIniPath, &self)) return false;
    char dll[MAX_PATH];
    DWORD len = GetModuleFileNameA(self, dll, MAX_PATH);
    if (!len || len >= MAX_PATH) return false;
    char* slash = strrchr(dll, '\\');
    if (!slash) return false;
    *slash = 0;
    snprintf(out, n, "%s\\axlelift.ini", dll);
    return true;
}

static bool DocumentsIniPath(char* out, size_t n)
{
    char dir[MAX_PATH];
    if (FAILED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, 0, dir))) return false;
    snprintf(out, n, "%s\\Euro Truck Simulator 2\\axlelift.ini", dir);
    return true;
}

static void Defaults(Settings& s)
{
    s = Settings{};
    for (unsigned i = 0; i < MAX_LIFT_AXLES; ++i) {
        s.axle[1][i].key = KeyBind{ (int)('1' + i), false, true, false };      // first trailer: Alt+1..8
        s.axle[0][i].key = KeyBind{ (int)('1' + i), true, true, false };       // truck: Ctrl+Alt+1..8
    }
    s.releaseAll.key = KeyBind{ '0', false, true, false };                     // Alt+0
}

static const char* ReverseName(ReverseRule r) { return r == ReverseRule::Lower ? "lower" : r == ReverseRule::Lift ? "lift" : "none"; }

static bool WriteIni(const char* path)
{
    FILE* f = fopen(path, "w");
    if (!f) return false;
    const Settings& s = g_settings;
    char t[160];
    fprintf(f, "; Lift Axle Extension %s settings. Edit in the overlay (default Alt+O) or here while the game is closed.\n", AXLELIFT_VERSION);
    fprintf(f, "; Keys: e.g. Alt+1, Ctrl+Shift+L, F6, Num3. Controller buttons are best set in the overlay.\n\n");
    fprintf(f, "settings_version=3\n");
    fprintf(f, "lift_seconds=%.2f\n", s.liftSeconds);
    KeyBindText(s.overlayKey, t, sizeof(t)); fprintf(f, "overlay_key=%s\n", t);
    fprintf(f, "auto_reverse=%s\n", ReverseName(s.autoReverse));
    fprintf(f, "overlay_mouse_speed=%.2f\n", s.overlayMouseSpeed);
    fprintf(f, "verbose_log=%d\n", s.verboseLog ? 1 : 0);
    KeyBindText(s.releaseAll.key, t, sizeof(t)); fprintf(f, "\nrelease_key=%s\n", t);
    JoySave(s.releaseAll.joy, t, sizeof(t)); fprintf(f, "release_button=%s\n", t);
    for (unsigned v = 0; v < VEHICLE_COUNT; ++v) {
        fprintf(f, "\n");
        for (unsigned i = 0; i < MAX_LIFT_AXLES; ++i) {
            KeyBindText(s.axle[v][i].key, t, sizeof(t)); fprintf(f, "%s.axle%u.key=%s\n", kSlotKeys[v], i + 1, t);
            if (s.axle[v][i].joy.valid()) { JoySave(s.axle[v][i].joy, t, sizeof(t)); fprintf(f, "%s.axle%u.button=%s\n", kSlotKeys[v], i + 1, t); }
            if (s.axleAuto[v][i]) fprintf(f, "%s.axle%u.auto=1\n", kSlotKeys[v], i + 1);
        }
    }
    fclose(f);
    return true;
}

static float Clampf(float v, float lo, float hi) { return std::min(std::max(v, lo), hi); }

static bool ReadIni(const char* path)
{
    FILE* f = fopen(path, "r");
    if (!f) return false;
    Settings& s = g_settings;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* nl = strpbrk(line, "\r\n"); if (nl) *nl = 0;
        char* eq = strchr(line, '=');
        if (line[0] == ';' || line[0] == '#' || !eq) continue;
        *eq = 0;
        const char* k = line; const char* val = eq + 1;
        unsigned idx;
        char slot[16];
        if (!strcmp(k, "settings_version")) (void)atoi(val);   // keys of removed features are simply ignored
        else if (!strcmp(k, "lift_seconds")) s.liftSeconds = Clampf((float)atof(val), 0.2f, 20.0f);
        else if (!strcmp(k, "overlay_key")) { KeyBind b = ParseKey(val); if (b.valid()) s.overlayKey = b; }
        else if (!strcmp(k, "auto_reverse")) s.autoReverse = !_stricmp(val, "lower") ? ReverseRule::Lower : !_stricmp(val, "lift") ? ReverseRule::Lift : ReverseRule::None;
        else if (!strcmp(k, "verbose_log")) s.verboseLog = atoi(val) != 0;
        else if (!strcmp(k, "overlay_mouse_speed")) s.overlayMouseSpeed = Clampf((float)atof(val), 0.1f, 10.0f);
        else if (!strcmp(k, "release_key") || !strcmp(k, "release")) s.releaseAll.key = ParseKey(val);   // "release" = older versions
        else if (!strcmp(k, "release_button")) s.releaseAll.joy = ParseJoy(val);
        else if (sscanf(k, "axle%u", &idx) == 1 && idx >= 1 && idx <= MAX_LIFT_AXLES && !strchr(k, '.'))
            s.axle[1][idx - 1].key = ParseKey(val);                                                       // older versions: first trailer
        else if (sscanf(k, "%15[a-z0-9].axle%u", slot, &idx) == 2 && idx >= 1 && idx <= MAX_LIFT_AXLES) {
            for (unsigned v = 0; v < VEHICLE_COUNT; ++v) {
                if (strcmp(slot, kSlotKeys[v]) != 0) continue;
                const char* field = strrchr(k, '.') + 1;
                if (!strcmp(field, "key")) s.axle[v][idx - 1].key = ParseKey(val);
                else if (!strcmp(field, "button")) s.axle[v][idx - 1].joy = ParseJoy(val);
                else if (!strcmp(field, "auto")) s.axleAuto[v][idx - 1] = atoi(val) != 0;
            }
        }
    }
    fclose(f);
    return true;
}

void SaveSettings()
{
    char path[MAX_PATH + 64];
    if (PluginIniPath(path, sizeof(path)) && WriteIni(path)) return;
    if (DocumentsIniPath(path, sizeof(path)) && WriteIni(path)) {
        Log(SCS_LOG_TYPE_warning, "[axlelift] plugins folder not writable - settings saved to %s", path);
        return;
    }
    Log(SCS_LOG_TYPE_error, "[axlelift] could not save settings");
}

void LoadSettings()
{
    Defaults(g_settings);
    char path[MAX_PATH + 64];
    if (PluginIniPath(path, sizeof(path)) && ReadIni(path)) {
        LogInfo("[axlelift] settings loaded from %s", path);
        return;
    }
    char old[MAX_PATH + 64];
    if (DocumentsIniPath(old, sizeof(old)) && ReadIni(old)) LogInfo("[axlelift] settings taken over from %s", old);
    SaveSettings(); // first start: create the file next to the DLL
}
