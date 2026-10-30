// human_ai: let the game's own AI run chosen parts of a human player's country.
//
// How it works (see README.md for the addresses this was derived from):
//  * The console command `human_ai` flips one global byte (g_humanAI). While it is
//    set, the engine runs the full country AI for human countries too.
//  * The country AI is a set of CAIModule subclasses ("ministers"/"generals"),
//    each updated through vtable slots labelled Module.HourlyUpdate, DailyUpdate...
//  * We set g_humanAI ourselves and patch those slots. For a human country, a
//    module's update only runs if the player handed that category to the AI.
//    AI countries are never affected.
#include "hai.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include <shlobj.h>

#include "aidata.h"
#include "defines.h"
#include "scan.h"

extern "C" void HaiThunk();

namespace hai {

// ---------------------------------------------------------------------------
// Logging (hoi4 folder\human_ai.log)
// ---------------------------------------------------------------------------
static char g_dir[MAX_PATH];  // folder holding hoi4.exe and this dll
static std::mutex g_log_mutex;

const char* Dir() { return g_dir; }

void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    char path[MAX_PATH];
    sprintf_s(path, "%s\\human_ai.log", g_dir);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") || !f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// ---------------------------------------------------------------------------
// Categories. Module classes map to one or more; land/air/navy are split
// further inside CAIMilitaryMinister by call-site hooks (see InstallTheatreHooks).
// Comments marked (?) are working assumptions still to verify in game.
// ---------------------------------------------------------------------------
enum Category { kForeign, kInterior, kPolitical, kLand, kAir, kNavy, kVolunteers, kCryptology, kCount };
constexpr unsigned Bit(Category c) { return 1u << c; }

struct ModuleClass {
    const char* rtti;
    unsigned mask;  // module runs for a human country if any of these is handed to the AI
};

static const char* const kCategoryKeys[kCount] = {
    "foreign",     // CAIForeignMinister: diplomacy, factions, guarantees, lend-lease (?)
    "interior",    // CAIInteriorMinister: trade, garrisons, equipment/ship designs (?)
    "political",   // CAIPoliticalMinister: focuses, laws, advisors, scientists (?)
    "land",        // CAIGeneral / CAIBaseGeneral: fronts, armies, army groups
    "air",         // CAIMilitaryMinister air sub-AI: air bases, air missions
    "navy",        // CAIMilitaryMinister naval sub-AI: naval objectives, fleets
    "volunteers",  // CAIVolunteerGeneral
    "cryptology",  // CCryptologyAI
};

// The military minister hosts the theatre, air and naval AI, so it has to run
// whenever any of land/air/navy is handed over; air and navy are then gated
// individually inside it.
static const ModuleClass kModuleClasses[] = {
    {"CAIForeignMinister", Bit(kForeign)},
    {"CAIInteriorMinister", Bit(kInterior)},
    {"CAIMilitaryMinister", Bit(kLand) | Bit(kAir) | Bit(kNavy)},
    {"CAIPoliticalMinister", Bit(kPolitical)},
    {"CAIGeneral", Bit(kLand)},
    {"CAIBaseGeneral", Bit(kLand)},
    {"CAIVolunteerGeneral", Bit(kVolunteers)},
    {"CCryptologyAI", Bit(kCryptology)},
};

// Profiler labels in the base CAIModule implementation of each update slot.
static const char* const kUpdateSlotLabels[] = {
    "Module.Update",       "Module.UpdateAIRoles", "Module.HourlyUpdate", "Module.DailyUpdate",
    "Module.WeeklyUpdate", "Module.MonthlyUpdate", "Module.YearlyUpdate",
};
constexpr int kNumUpdateSlots = sizeof(kUpdateSlotLabels) / sizeof(kUpdateSlotLabels[0]);

// ---------------------------------------------------------------------------
// Config (hoi4 folder\human_ai.ini), hot-reloaded.
// ---------------------------------------------------------------------------
static std::atomic<bool> g_enabled{false};
static std::atomic<bool> g_ai_controls[kCount];

static unsigned AIMask() {
    unsigned m = 0;
    for (int c = 0; c < kCount; ++c)
        if (g_ai_controls[c]) m |= Bit((Category)c);
    return m;
}

static const char kDefaultIni[] =
    "; human_ai - hand parts of your country to the game's own AI.\n"
    "; Saved changes are picked up within a second, mid-game.\n"
    ";   1 = the AI runs this for you, 0 = you do.\n"
    "[human_ai]\n"
    "enabled=1\n"
    "foreign=0\n"
    "interior=0\n"
    "political=0\n"
    "land=0\n"
    "air=0\n"
    "navy=0\n"
    "volunteers=0\n"
    "cryptology=0\n"
    "; open the HumanAI settings window when the game starts\n"
    "launch_app=1\n"
    "; optional AI pack: a folder in human_ai_packs (empty = vanilla AI)\n"
    "ai_pack=\n";

static void IniPath(char* out) { sprintf_s(out, MAX_PATH, "%s\\human_ai.ini", g_dir); }

static std::mutex g_pack_mutex;
static std::string g_pack;  // folder name in human_ai_packs, empty = none

static void LoadConfig() {
    char ini[MAX_PATH];
    IniPath(ini);
    g_enabled = GetPrivateProfileIntA("human_ai", "enabled", 1, ini) != 0;
    char pack[MAX_PATH] = {};
    GetPrivateProfileStringA("human_ai", "ai_pack", "", pack, MAX_PATH, ini);
    {
        std::lock_guard<std::mutex> lock(g_pack_mutex);
        g_pack = pack;
    }
    char line[256] = "config:";
    for (int c = 0; c < kCount; ++c) {
        g_ai_controls[c] = GetPrivateProfileIntA("human_ai", kCategoryKeys[c], 0, ini) != 0;
        sprintf_s(line + strlen(line), sizeof(line) - strlen(line), " %s=%d", kCategoryKeys[c],
                  (int)g_ai_controls[c].load());
    }
    Log("%s enabled=%d ai_pack=%s", line, (int)g_enabled.load(), pack[0] ? pack : "(none)");
}

static FILETIME IniWriteTime() {
    char ini[MAX_PATH];
    IniPath(ini);
    WIN32_FILE_ATTRIBUTE_DATA a{};
    GetFileAttributesExA(ini, GetFileExInfoStandard, &a);
    return a.ftLastWriteTime;
}

// ---------------------------------------------------------------------------
// Game functions and globals, resolved by signature at startup.
// ---------------------------------------------------------------------------
using GetCountry_t = void*(__fastcall*)(void* country_ai);
using IsAITag_t = bool(__fastcall*)(void* gamestate, const void* tag);

static volatile uint8_t* g_humanAI = nullptr;  // the `human_ai` console toggle
static void** g_gamestate = nullptr;            // pointer global passed to IsAITag
static IsAITag_t IsAITag = nullptr;             // true = AI-controlled (ignores g_humanAI)
static GetCountry_t CountryAI_GetCountry = nullptr;
constexpr size_t kCountryTagOffset = 8;         // CCountry: tag handle at +8

static bool Resolve() {
    using scan::img;
    // is_ai trigger: isAI = IsAITag(*gamestate, &scope.tag) || g_humanAI
    //   48 8B 0D <gamestate>  48 8D 56 08  E8 <IsAITag>  48 8B 74 24 50  84 C0  75 ??  38 05 <g_humanAI>
    auto hits = scan::find(img.text,
                           "48 8B 0D ?? ?? ?? ?? 48 8D 56 08 E8 ?? ?? ?? ?? 48 8B 74 24 50 84 C0 75 ?? 38 05");
    if (hits.size() != 1) {
        Log("resolve: is_ai trigger signature matched %zu times (need 1)", hits.size());
        return false;
    }
    uint8_t* p = hits[0];
    g_gamestate = reinterpret_cast<void**>(scan::rel32(p + 3));
    IsAITag = reinterpret_cast<IsAITag_t>(scan::rel32(p + 12));
    auto flag_from_trigger = scan::rel32(p + 27);

    // Cross-check against the console command handlers. Every toggle command has the shape
    //   cmp [flag], al ; sete al ; movzx r9d, al ; mov [flag], al
    // so one of them must toggle the same byte.
    bool handler_agrees = false;
    for (uint8_t* h : scan::find(img.text, "38 05 ?? ?? ?? ?? 0F 94 C0 44 0F B6 C8 88 05"))
        handler_agrees |= scan::rel32(h + 2) == flag_from_trigger;
    if (!handler_agrees) {
        Log("resolve: no console toggle writes the is_ai trigger's flag (+%#llx)", flag_from_trigger - img.base);
        return false;
    }
    g_humanAI = flag_from_trigger;

    // Module updates open with (r15 = this)
    //   mov r15,rcx ; mov rcx,[rcx+8] ; mov rax,[rcx] ; call [rax+8] ; mov rcx,rax ; call CCountryAI::GetCountry
    // Several functions do this; all must call the same GetCountry.
    hits = scan::find(img.text, "4C 8B F9 48 8B 49 08 48 8B 01 FF 50 08 48 8B C8 E8");
    for (uint8_t* h : hits) {
        auto fn = reinterpret_cast<GetCountry_t>(scan::rel32(h + 17));
        if (CountryAI_GetCountry && fn != CountryAI_GetCountry) {
            Log("resolve: GetCountry signature is ambiguous");
            return false;
        }
        CountryAI_GetCountry = fn;
    }
    if (!CountryAI_GetCountry) {
        Log("resolve: GetCountry signature not found");
        return false;
    }
    Log("resolve: g_humanAI=+%#llx gamestate=+%#llx IsAITag=+%#llx GetCountry=+%#llx (%zu GetCountry hits)",
        (uint8_t*)g_humanAI - img.base, (uint8_t*)g_gamestate - img.base, (uint8_t*)IsAITag - img.base,
        (uint8_t*)CountryAI_GetCountry - img.base, hits.size());
    return true;
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------
enum HookKind { kModuleHook, kPostHook };

struct HookCtx {
    void* original;   // must stay first: HaiThunk jumps through [r10]
    unsigned mask;    // categories that let this run for a human country
    ptrdiff_t adjust; // the hooked function's rcx is module + adjust
    int slot;         // vtable slot, or -1 for a call-site hook
    const char* name;
    std::atomic<bool> logged;
    HookKind kind;
};

static HookCtx g_hooks[sizeof(kModuleClasses) / sizeof(kModuleClasses[0])][kNumUpdateSlots];

static void MaskNames(unsigned mask, char* out, size_t n) {
    out[0] = 0;
    for (int c = 0; c < kCount; ++c)
        if (mask & Bit((Category)c))
            sprintf_s(out + strlen(out), n - strlen(out), "%s%s", out[0] ? "|" : "", kCategoryKeys[c]);
}
static std::atomic<bool> g_chain_bad{false};  // owner/AI/country chain did not look right: stand down
static std::atomic<bool> g_resolved{false};
static std::atomic<int> g_module_hook_count{0};
static std::atomic<int> g_site_hook_count{0};

// Find module -> CCountry. Every step is checked so a layout change fails open.
static void* ModuleCountry(void* module, bool verbose) {
    __try {
        void* owner = *reinterpret_cast<void**>((uint8_t*)module + 8);
        if (!owner) return nullptr;
        auto vt = *reinterpret_cast<void* (__fastcall***)(void*)>(owner);
        void* country_ai = vt[1](owner);
        if (!country_ai) return nullptr;
        const char* ai_cls = scan::class_name(country_ai);
        if (verbose)
            Log("  chain: module=%s owner=%s ai=%s", scan::class_name(module), scan::class_name(owner),
                ai_cls ? ai_cls : "?");
        if (!ai_cls || strcmp(ai_cls, "CCountryAI@@") != 0) {
            if (!g_chain_bad.exchange(true))
                Log("chain: expected CCountryAI, got %s; leaving all modules to vanilla", ai_cls ? ai_cls : "?");
            return nullptr;
        }
        return CountryAI_GetCountry(country_ai);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (!g_chain_bad.exchange(true)) Log("chain: access violation; leaving all modules to vanilla");
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// Multiplayer. HoI4 runs the AI only on the host (ai_update is skipped unless the
// session's "runs AI" byte is set), and every AI decision is posted as a CCommand
// through the same path as player input, which the network layer forwards. So:
//  * never touch g_humanAI in multiplayer: it changes local script/order logic
//    and desyncs (seen as "Orders Groups");
//  * let CCountryAI::Update through for the local player's country only;
//  * on a client, force ai_update to run while an area is handed to the AI, but
//    let only the local country's modules run and only its AI commands out.
// The other players need nothing installed: they receive our AI's decisions as
// ordinary commands from us. See native/README.md.
// ---------------------------------------------------------------------------
enum Role { kRoleNone, kRoleSingle, kRoleHost, kRoleClient };
static const char* const kRoleNames[] = {"none", "single-player", "host", "client"};

using Getter_t = void*(__fastcall*)(void*);
using GateFn_t = bool(__fastcall*)(void*);

static void** g_session = nullptr;       // global session pointer
static int g_ctx_vfunc = 0;              // session vtable offset: command context
static int g_player_vfunc = 0;           // session vtable offset: pointer to the local player's tag
static int g_runs_ai_offset = 0;         // command context: "this machine runs the AI" byte
constexpr int kServerOffset = 0x40;      // command context: CServer subclass
static uint8_t* g_sched_jump = nullptr;  // je that skips ai_update when not running the AI
static uint8_t g_sched_orig[2];
static GateFn_t g_gate_orig = nullptr;   // "is AI or human_ai" check in CCountryAI::Update
static Getter_t PostingCountry = nullptr;
static bool g_mp_ready = false;

static std::atomic<int> g_role{kRoleNone};
static std::atomic<int> g_mine_tag{0};  // local player's country tag (0 = none)
static std::atomic<int> g_dropped{0};

static Getter_t VFunc(void* obj, int offset) {
    return reinterpret_cast<Getter_t*>(*reinterpret_cast<void***>(obj))[offset / 8];
}

static int CountryTag(void* country) {
    return *reinterpret_cast<int32_t*>((uint8_t*)country + kCountryTagOffset);
}

static bool IsMine(void* country) {
    int tag = g_mine_tag;
    return country && tag != 0 && CountryTag(country) == tag;
}

// Called from our own thread every 250 ms.
static void UpdateRole() {
    // Without the multiplayer hooks we can't tell; behave as single-player did before.
    int role = g_session ? kRoleNone : kRoleSingle;
    int tag = 0;
    __try {
        void* s = g_session ? *g_session : nullptr;
        if (s) {
            void* ctx = VFunc(s, g_ctx_vfunc)(s);
            void* server = ctx ? *reinterpret_cast<void**>((uint8_t*)ctx + kServerOffset) : nullptr;
            const char* n = server ? scan::class_name(server) : nullptr;
            if (n && !strcmp(n, "CProxyServer@@")) role = kRoleClient;
            else if (n && (!strcmp(n, "CNetworkServer@@") || !strcmp(n, "CDedicatedServer@@"))) role = kRoleHost;
            else if (n && !strcmp(n, "CDummyServer@@")) role = kRoleSingle;
            if (auto* t = reinterpret_cast<int32_t*>(VFunc(s, g_player_vfunc)(s))) tag = *t;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        role = kRoleNone;
        tag = 0;
    }
    if (g_role.exchange(role) != role || g_mine_tag.exchange(tag) != tag)
        Log("session: %s, player tag %d", kRoleNames[role], tag);
}

static bool Multiplayer() {
    int r = g_role;
    return g_mp_ready && (r == kRoleHost || r == kRoleClient);
}

// Replaces the "is AI (or human_ai)" call at the top of CCountryAI::Update.
extern "C" bool __fastcall HaiCountryGate(void* country) {
    bool orig = g_gate_orig(country);
    if (!g_enabled || g_chain_bad || !Multiplayer()) return orig;
    if (IsMine(country)) return AIMask() != 0;
    return g_role == kRoleHost ? orig : false;
}

// On a client the AI normally never runs; force ai_update while we need it.
static void SetJumpBytes(const uint8_t b[2]) {
    if (!g_sched_jump || (g_sched_jump[0] == b[0] && g_sched_jump[1] == b[1])) return;
    uintptr_t at = (uintptr_t)g_sched_jump;
    auto* q = reinterpret_cast<volatile LONG64*>(at & ~(uintptr_t)7);
    size_t off = at & 7;
    if (off > 6) return;  // straddles two qwords: refuse rather than tear the instruction
    DWORD old;
    if (!VirtualProtect((void*)q, 8, PAGE_EXECUTE_READWRITE, &old)) return;
    LONG64 cur, next;
    do {
        cur = *q;
        next = cur;
        memcpy(reinterpret_cast<uint8_t*>(&next) + off, b, 2);
    } while (InterlockedCompareExchange64(q, next, cur) != cur);
    VirtualProtect((void*)q, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)q, 8);
}

static void SyncScheduler() {
    static const uint8_t nops[2] = {0x90, 0x90};
    bool force = g_mp_ready && g_enabled && !g_chain_bad && g_role == kRoleClient && g_mine_tag != 0 && AIMask();
    SetJumpBytes(force ? nops : g_sched_orig);
}

static bool ShouldPost(void* cmd, void* posting) {
    if (!g_enabled || g_role != kRoleClient || !g_mp_ready) return true;
    void* country = nullptr;
    __try { country = PostingCountry(posting); } __except (EXCEPTION_EXECUTE_HANDLER) { country = nullptr; }
    if (IsMine(country)) return true;
    if (g_dropped++ < 20)
        Log("mp: dropped AI command %s for another country (%p)", scan::class_name(cmd) ? scan::class_name(cmd) : "?",
            country);
    return false;
}

extern "C" bool HaiShouldRun(void* self, HookCtx* ctx, void* arg2) {
    if (ctx->kind == kPostHook) return ShouldPost(self, arg2);
    if (!g_enabled || g_chain_bad) return true;
    bool first = !ctx->logged.exchange(true);
    void* country = ModuleCountry((uint8_t*)self - ctx->adjust, first);
    if (!country) return true;
    bool ai_country = IsAITag(*g_gamestate, (uint8_t*)country + kCountryTagOffset);
    bool run;
    if (Multiplayer()) {
        if (IsMine(country)) run = (AIMask() & ctx->mask) != 0;
        else run = g_role == kRoleHost && ai_country;  // a client never runs anyone else's AI
    } else {
        run = ai_country || (AIMask() & ctx->mask);
    }
    if (first) {
        char cats[96];
        MaskNames(ctx->mask, cats, sizeof(cats));
        Log("first call %s slot %d (%s): %s ai_country=%d mine=%d run=%d", ctx->name, ctx->slot, cats,
            kRoleNames[g_role.load()], ai_country, IsMine(country), run);
    }
    return run;
}

// Stubs must sit within +-2GB of the exe so call rel32 sites can reach them.
static uint8_t* AllocNearImage(size_t size) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity;
    const uintptr_t base = (uintptr_t)scan::img.base;
    for (uintptr_t d = gran; d < 0x70000000; d += gran) {
        if (base > d) {
            if (void* p = VirtualAlloc((void*)(base - d), size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))
                return (uint8_t*)p;
        }
        uintptr_t up = (uintptr_t)scan::img.data.end + d;
        if (void* p = VirtualAlloc((void*)(up & ~(gran - 1)), size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))
            return (uint8_t*)p;
    }
    return nullptr;
}

// mov r10, ctx ; mov rax, target ; jmp rax   (ctx may be null for plain redirects)
static void* MakeStubTo(void* ctx, void* target) {
    constexpr size_t kPage = 64 * 1024, kStub = 32;
    static uint8_t* page = nullptr;
    static size_t used = 0;
    if (!page) page = AllocNearImage(kPage);
    if (!page || used + kStub > kPage) return nullptr;
    uint8_t* s = page + used;
    used += kStub;
    s[0] = 0x49; s[1] = 0xBA; *reinterpret_cast<void**>(s + 2) = ctx;
    s[10] = 0x48; s[11] = 0xB8; *reinterpret_cast<void**>(s + 12) = target;
    s[20] = 0xFF; s[21] = 0xE0;
    return s;
}

static void* MakeStub(HookCtx* ctx) { return MakeStubTo(ctx, (void*)&HaiThunk); }

// Point a `call rel32` at `target` through a stub near the image.
static bool PatchCall(uint8_t* call, void* stub) {
    long long rel = (uint8_t*)stub - (call + 5);
    if (!stub || call[0] != 0xE8 || rel != (int32_t)rel) return false;
    DWORD old;
    if (!VirtualProtect(call + 1, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    *reinterpret_cast<int32_t*>(call + 1) = (int32_t)rel;
    VirtualProtect(call + 1, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    return true;
}

static bool PatchSlot(void** slot, void* value) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *slot = value;
    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

static bool InstallHooks(bool patch = true) {
    void** base = scan::vtable("CAIModule");
    if (!base) {
        Log("hooks: CAIModule vtable not found");
        return false;
    }
    int slot_index[kNumUpdateSlots];
    for (int u = 0; u < kNumUpdateSlots; ++u) {
        slot_index[u] = -1;
        for (int i = 0; i < 32 && scan::img.text.contains(base[i]); ++i) {
            if (scan::references_string((const uint8_t*)base[i], 0x40, kUpdateSlotLabels[u])) {
                slot_index[u] = i;
                break;
            }
        }
        Log("hooks: %s = slot %d", kUpdateSlotLabels[u], slot_index[u]);
    }

    int installed = 0;
    for (size_t c = 0; c < sizeof(kModuleClasses) / sizeof(kModuleClasses[0]); ++c) {
        void** vt = scan::vtable(kModuleClasses[c].rtti);
        if (!vt) {
            Log("hooks: %s vtable not found, skipped", kModuleClasses[c].rtti);
            continue;
        }
        for (int u = 0; u < kNumUpdateSlots; ++u) {
            if (slot_index[u] < 0) continue;
            HookCtx& h = g_hooks[c][u];
            h.original = vt[slot_index[u]];
            h.mask = kModuleClasses[c].mask;
            h.adjust = 0;
            h.slot = slot_index[u];
            h.name = kModuleClasses[c].rtti;
            if (!patch) {
                ++installed;
                continue;
            }
            void* stub = MakeStub(&h);
            if (stub && PatchSlot(&vt[slot_index[u]], stub)) ++installed;
        }
    }
    Log("hooks: %d slots %s", installed, patch ? "patched" : "found (dry run)");
    g_module_hook_count = installed;
    return installed > 0;
}

// CAIMilitaryMinister's theatre update (called from its Hourly and Daily updates,
// rdi = minister) runs the air and naval sub-AIs back to back:
//   +0   mov rdx,[rdi+?] ; lea rcx,[rdi+AIR] ; mov r8,rax ; mov rbx,rax
//   +20  call air_bases(air)                 (air base access)
//   +25  mov rdx,[rdi+?] ; lea rcx,[rdi+AIR] ; mov r8,rbx
//   +42  call air_plan(air)                  (mission planning, interceptor reserve)
//   +50  call ?(this)  +58 call air_scores(this)  +66 call ?(this)  +74 call air_scores2(this)
//   +79  lea rcx,[rdi+NAVY]
//   +86  call naval_objectives(navy)         (naval goals, home bases)
//   +91  lea rcx,[rdi+NAVY]
//   +98  call naval_ready(navy) -> bool      (false skips the whole naval block)
//   +103 test al,al
// Air calls return nothing; a skipped naval_ready returns false, so skipping is safe.
struct CallSite {
    int call_offset;   // offset of the E8 in the signature match
    int adjust_disp;   // offset of the lea rcx disp32 giving the sub-object, or -1 for rcx = this
    Category cat;
    const char* name;
};
static const CallSite kTheatreSites[] = {
    {20, 10, kAir, "air: bases"},          {42, 35, kAir, "air: mission plan"},
    {58, -1, kAir, "air: mission scores"}, {74, -1, kAir, "air: mission scores 2"},
    {86, 82, kNavy, "navy: objectives"},   {98, 94, kNavy, "navy: fleet block"},
};
constexpr int kNumTheatreSites = sizeof(kTheatreSites) / sizeof(kTheatreSites[0]);
static HookCtx g_site_hooks[kNumTheatreSites];

static bool InstallTheatreHooks(bool patch) {
    auto hits = scan::find(scan::img.text,
        "48 8B 97 ?? ?? ?? ?? 48 8D 8F ?? ?? ?? ?? 4C 8B C0 48 8B D8 E8 ?? ?? ?? ?? "
        "48 8B 97 ?? ?? ?? ?? 48 8D 8F ?? ?? ?? ?? 4C 8B C3 E8 ?? ?? ?? ?? "
        "48 8B CF E8 ?? ?? ?? ?? 48 8B CF E8 ?? ?? ?? ?? 48 8B CF E8 ?? ?? ?? ?? 48 8B CF E8 ?? ?? ?? ?? "
        "48 8D 8F ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8D 8F ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0");
    if (hits.size() != 1) {
        Log("theatre: signature matched %zu times (need 1); air and navy follow land", hits.size());
        return false;
    }
    uint8_t* p = hits[0];
    int done = 0;
    for (int i = 0; i < kNumTheatreSites; ++i) {
        const CallSite& s = kTheatreSites[i];
        uint8_t* call = p + s.call_offset;
        HookCtx& h = g_site_hooks[i];
        h.original = scan::rel32(call + 1);
        h.mask = Bit(s.cat);
        h.adjust = s.adjust_disp < 0 ? 0 : *reinterpret_cast<int32_t*>(p + s.adjust_disp);
        h.slot = -1;
        h.name = s.name;
        Log("theatre: %-22s call +%#llx -> +%#llx (rcx = minister + %#llx)", s.name, call - scan::img.base,
            (uint8_t*)h.original - scan::img.base, (long long)h.adjust);
        if (!patch) {
            ++done;
            continue;
        }
        uint8_t* stub = (uint8_t*)MakeStub(&h);
        if (!stub) continue;
        long long rel = stub - (call + 5);
        if (rel != (int32_t)rel) {
            Log("theatre: stub out of rel32 range");
            continue;
        }
        DWORD old;
        if (!VirtualProtect(call + 1, 4, PAGE_EXECUTE_READWRITE, &old)) continue;
        *reinterpret_cast<int32_t*>(call + 1) = (int32_t)rel;
        VirtualProtect(call + 1, 4, old, &old);
        FlushInstructionCache(GetCurrentProcess(), call, 5);
        ++done;
    }
    Log("theatre: %d/%d call sites %s", done, kNumTheatreSites, patch ? "patched" : "found (dry run)");
    g_site_hook_count = done;
    return done == kNumTheatreSites;
}

// ---------------------------------------------------------------------------
// Status for the HumanAI settings window (human_ai_status.ini, next to the dll).
// ---------------------------------------------------------------------------
static void WriteStatus(const char* state) {
    char path[MAX_PATH], v[32];
    sprintf_s(path, "%s\\human_ai_status.ini", g_dir);
    WritePrivateProfileStringA("status", "version", HAI_VERSION, path);
    WritePrivateProfileStringA("status", "state", state, path);  // active | unsupported | stood_down
    sprintf_s(v, "%lu", GetCurrentProcessId());
    WritePrivateProfileStringA("status", "pid", v, path);
    sprintf_s(v, "%d", g_module_hook_count.load());
    WritePrivateProfileStringA("status", "module_hooks", v, path);
    sprintf_s(v, "%d", g_site_hook_count.load());
    WritePrivateProfileStringA("status", "site_hooks", v, path);
    WritePrivateProfileStringA("status", "engine_flag", (g_humanAI && *g_humanAI) ? "1" : "0", path);
}

// ---------------------------------------------------------------------------
// AI packs (optional add-on): human_ai_packs\<name> holds any AI mod's files. Only
// its AI defines are applied, in memory, once a game is loaded (after the game has
// read its own defines). See defines.cpp.
// ---------------------------------------------------------------------------
static bool g_defines_ready = false;

static void WritePackStatus(const std::string& pack, const defines::ApplyResult& r) {
    char path[MAX_PATH], v[32];
    sprintf_s(path, "%s\\human_ai_status.ini", g_dir);
    WritePrivateProfileStringA("pack", "name", pack.c_str(), path);
    sprintf_s(v, "%d", r.applied);
    WritePrivateProfileStringA("pack", "applied", v, path);
    sprintf_s(v, "%d", r.skipped);
    WritePrivateProfileStringA("pack", "skipped", v, path);
    sprintf_s(v, "%d", r.ignored);
    WritePrivateProfileStringA("pack", "ignored", v, path);
}

static void SyncPack(int role) {
    static std::string applied;
    static bool in_game = false;
    if (!g_defines_ready) return;
    std::string want;
    {
        std::lock_guard<std::mutex> lock(g_pack_mutex);
        want = g_pack;
    }
    bool game = role != kRoleNone;
    if (game == in_game && want == applied) return;
    if (!game) {  // only apply inside a game; the game reads its defines at startup
        in_game = false;
        return;
    }
    in_game = true;
    applied = want;
    if (want.empty()) {
        defines::Restore();
        WritePackStatus("", {});
        return;
    }
    std::string dir = std::string(g_dir) + "\\human_ai_packs\\" + want;
    defines::ApplyResult r = defines::ApplyPack(dir);
    Log("pack '%s': %d AI defines applied (%d arrays), %d not in this game version, %d gameplay defines ignored, "
        "%d arrays not applicable",
        want.c_str(), r.applied, r.arrays, r.skipped, r.ignored, r.tables);
    WritePackStatus(want, r);
}

static uint8_t* FuncStart(uint8_t* p) {  // MSVC pads between functions with int3
    while (p > scan::img.text.begin && !(p[-1] == 0xCC && p[-2] == 0xCC)) --p;
    return p;
}

static HookCtx g_post_hooks[2];

static bool InstallMultiplayerHooks(bool patch) {
    using scan::img;
    // Scheduler: if (... && session->ctx()->runs_ai) ai_update(...)
    //   48 8B 0D <session> 48 8B 01 FF 90 <ctx vfunc> 80 78 <runs_ai> 00 74 ?? 48 8D 54 24 58 49 8B CF E8 <ai_update>
    auto hits = scan::find(img.text, "48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 ?? ?? 00 00 80 78 ?? 00 74 ?? 48 8D 54 24 58 49 8B CF E8");
    if (hits.size() != 1) {
        Log("mp: scheduler signature matched %zu times; multiplayer support off", hits.size());
        return false;
    }
    uint8_t* p = hits[0];
    g_session = reinterpret_cast<void**>(scan::rel32(p + 3));
    g_ctx_vfunc = *reinterpret_cast<int32_t*>(p + 12);
    g_runs_ai_offset = p[18];
    g_sched_jump = p + 20;
    memcpy(g_sched_orig, g_sched_jump, 2);

    // Local player's tag: `mov rcx,[session] ; mov rax,[rcx] ; call [rax+X]` whose result is then read
    // as a dword (`mov r?d,[rax]`). The game does this at ~20 places; all must agree.
    // (Not the object the human_ai popups fetch: that one isn't a country, as multiplayer testing showed.)
    for (const char* pat : {"48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 ?? ?? 00 00 8B ?? 44 8B 00",
                            "48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 ?? ?? 00 00 8B 08",
                            "48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 ?? ?? 00 00 8B 00"}) {
        for (uint8_t* h : scan::find(img.text, pat)) {
            if (scan::rel32(h + 3) != (uint8_t*)g_session) continue;
            int off = *reinterpret_cast<int32_t*>(h + 12);
            if (g_player_vfunc && off != g_player_vfunc) {
                Log("mp: player-tag lookup is ambiguous; multiplayer support off");
                return false;
            }
            g_player_vfunc = off;
        }
    }

    // AI command posting: the functions asserting IsCommandsAllowed that take (cmd, posting country)
    // and convert it with `mov rcx,rdx ; call <PostingCountry>`.
    uint8_t* post_fn[2] = {};
    int n_post = 0;
    auto strs = scan::find(img.rdata, "50 6F 73 74 69 6E 67 43 6F 75 6E 74 72 79 2E 47 65 74 43 6F 75 6E 74 72 79 28 29 2E 47 65 74 41 49 28 29 2D 3E 49 73 43 6F 6D 6D 61 6E 64 73 41 6C 6C 6F 77 65 64 28 29 00");
    for (uint8_t* s : strs) {
        for (uint8_t* h : scan::find(img.text, "48 8D 0D")) {
            if (scan::rel32(h + 3) != s) continue;
            uint8_t* f = FuncStart(h);
            auto conv = scan::find(scan::Range{f, f + 0x30}, "48 8B CA E8");
            if (conv.empty()) continue;  // the other poster has a different shape; leave it
            auto pc = reinterpret_cast<Getter_t>(scan::rel32(conv[0] + 4));
            if (PostingCountry && pc != PostingCountry) continue;
            PostingCountry = pc;
            bool dup = false;
            for (int i = 0; i < n_post; ++i) dup |= post_fn[i] == f;
            if (!dup && n_post < 2) post_fn[n_post++] = f;
        }
    }

    // CCountryAI::Update: cmp [country+0x484],0 ; jle ; mov rcx,rax ; call <IsAI or human_ai>
    uint8_t* gate_call = nullptr;
    if (void** vt = scan::vtable("CCountryAI")) {
        for (int i = 0; i < 16 && !gate_call && img.text.contains(vt[i]); ++i) {
            auto g = scan::find(scan::Range{(uint8_t*)vt[i], (uint8_t*)vt[i] + 0x40}, "83 B8 ?? ?? 00 00 00 7E ?? 48 8B C8 E8");
            if (!g.empty()) gate_call = g[0] + 12;
        }
    }

    if (!g_player_vfunc || !PostingCountry || n_post == 0 || !gate_call || g_sched_orig[0] != 0x74) {
        Log("mp: missing pieces (player %d, posters %d, gate %p, jump %02x); multiplayer support off", g_player_vfunc,
            n_post, gate_call, g_sched_orig[0]);
        return false;
    }
    g_gate_orig = reinterpret_cast<GateFn_t>(scan::rel32(gate_call + 1));
    Log("mp: session=+%#llx ctx vfunc %#x, runs-AI byte +%#x, player vfunc %#x, scheduler jump +%#llx, gate +%#llx -> +%#llx",
        (uint8_t*)g_session - img.base, g_ctx_vfunc, g_runs_ai_offset, g_player_vfunc, g_sched_jump - img.base,
        gate_call - img.base, (uint8_t*)g_gate_orig - img.base);

    // Every direct call to the AI posters goes through one stub each.
    int sites = 0, patched = 0;
    for (int i = 0; i < n_post; ++i) {
        HookCtx& h = g_post_hooks[i];
        h.original = post_fn[i];
        h.kind = kPostHook;
        h.name = "ai post";
        h.slot = -1;
        void* stub = patch ? MakeStub(&h) : nullptr;
        for (uint8_t* c = img.text.begin; c + 5 <= img.text.end; ++c) {
            c = static_cast<uint8_t*>(memchr(c, 0xE8, img.text.end - 5 - c));
            if (!c) break;
            if (c + 5 + *reinterpret_cast<int32_t*>(c + 1) != post_fn[i]) continue;
            ++sites;
            if (patch && PatchCall(c, stub)) ++patched;
        }
        Log("mp: AI poster +%#llx", post_fn[i] - img.base);
    }
    if (patch && patched != sites) Log("mp: only %d of %d AI post calls patched", patched, sites);
    if (patch && !PatchCall(gate_call, MakeStubTo(nullptr, (void*)&HaiCountryGate))) {
        Log("mp: could not patch the country gate; multiplayer support off");
        return false;
    }
    Log("mp: %d AI post call sites %s, country gate %s", sites, patch ? "patched" : "found (dry run)",
        patch ? "patched" : "found");
    g_mp_ready = patch;
    return true;
}

// Keep the engine's human_ai switch in step with the config: on only while at
// least one category is handed to the AI, and never in multiplayer (it desyncs).
static void SyncHumanAIFlag() {
    int role = g_role;
    bool single = role == kRoleSingle || (!g_mp_ready && role != kRoleNone);
    uint8_t want = (single && g_enabled && !g_chain_bad && AIMask()) ? 1 : 0;
    if (*g_humanAI != want) {
        *g_humanAI = want;
        Log("human_ai engine flag -> %d", want);
        WriteStatus(g_chain_bad ? "stood_down" : "active");
    }
}

// Start HumanAI.exe (next to the dll) without taking focus from the game. It
// watches our pid and closes when the game exits; it also refuses a second copy.
static void LaunchApp() {
    char ini[MAX_PATH], exe[MAX_PATH], cmd[MAX_PATH + 32];
    IniPath(ini);
    if (!GetPrivateProfileIntA("human_ai", "launch_app", 1, ini)) return;
    sprintf_s(exe, "%s\\HumanAI.exe", g_dir);
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) {
        Log("app: %s not found, not launched", exe);
        return;
    }
    sprintf_s(cmd, "\"%s\" --pid %lu", exe, GetCurrentProcessId());
    STARTUPINFOA si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNOACTIVATE;
    PROCESS_INFORMATION pi{};
    if (CreateProcessA(exe, cmd, nullptr, nullptr, FALSE, 0, nullptr, g_dir, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        Log("app: launched");
    } else {
        Log("app: launch failed (error %lu)", GetLastError());
    }
}

// ---------------------------------------------------------------------------
// In-game panel (the Workshop mod in workshop/). Its buttons run
// `log = "HAI_CMD <key> <0|1>"`, which the game writes to logs/game.log; loading a
// save or opening the panel re-sends the campaign's choices the same way. We tail
// the file and store each choice in human_ai.ini, which the settings window mirrors.
// ---------------------------------------------------------------------------
struct LogTail {
    char path[MAX_PATH] = {};
    LONGLONG pos = -1;  // -1: not opened yet; start at the current end
    std::string partial;
};

static bool GameLogPath(char* out) {
    PWSTR docs = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) return false;
    char a[MAX_PATH];
    int n = WideCharToMultiByte(CP_ACP, 0, docs, -1, a, MAX_PATH, nullptr, nullptr);
    CoTaskMemFree(docs);
    if (n <= 0) return false;
    sprintf_s(out, MAX_PATH, "%s\\Paradox Interactive\\Hearts of Iron IV\\logs\\game.log", a);
    return true;
}

static void SetFromPanel(int c, bool ai) {
    if (g_ai_controls[c] == ai) return;
    g_ai_controls[c] = ai;
    char ini[MAX_PATH];
    IniPath(ini);
    WritePrivateProfileStringA("human_ai", kCategoryKeys[c], ai ? "1" : "0", ini);
    Log("panel: %s -> %s", kCategoryKeys[c], ai ? "AI" : "player");
}

static void HandleLogLine(const char* line) {
    const char* p = strstr(line, "HAI_CMD ");
    if (!p) return;
    char key[32];
    int value;
    if (sscanf_s(p + 8, "%31s %d", key, (unsigned)sizeof(key), &value) != 2) return;
    for (int c = 0; c < kCount; ++c)
        if (!strcmp(key, kCategoryKeys[c])) SetFromPanel(c, value != 0);
}

static void PollGameLog(LogTail& t) {
    HANDLE h = CreateFileA(t.path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size;
    if (GetFileSizeEx(h, &size)) {
        if (t.pos < 0) t.pos = size.QuadPart;  // ignore the previous session's lines
        if (size.QuadPart < t.pos) {           // the game truncated it for a new session
            t.pos = 0;
            t.partial.clear();
        }
        LARGE_INTEGER at;
        at.QuadPart = t.pos;
        char buf[64 * 1024];
        DWORD got = 0;
        if (SetFilePointerEx(h, at, nullptr, FILE_BEGIN)) {
            while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got) {
                t.pos += got;
                t.partial.append(buf, got);
            }
        }
        size_t start = 0, nl;
        while ((nl = t.partial.find('\n', start)) != std::string::npos) {
            t.partial[nl] = 0;
            HandleLogLine(t.partial.c_str() + start);
            start = nl + 1;
        }
        t.partial.erase(0, start);
    }
    CloseHandle(h);
}

DWORD WINAPI InitThread(LPVOID dll_module) {
    GetModuleFileNameA((HMODULE)dll_module, g_dir, MAX_PATH);
    if (char* slash = strrchr(g_dir, '\\')) *slash = 0;

    char ini[MAX_PATH];
    IniPath(ini);
    if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) {
        FILE* f = nullptr;
        if (!fopen_s(&f, ini, "w") && f) {
            fputs(kDefaultIni, f);
            fclose(f);
        }
    }

    Log("human_ai " HAI_VERSION " loading");
    LoadConfig();
    if (!scan::img.init()) return 0;
    {
        // First, before the slower scans: the loading screen reads the AI databases soon after launch.
        std::string pack;
        {
            std::lock_guard<std::mutex> lock(g_pack_mutex);
            pack = g_pack;
        }
        if (!pack.empty() && !aidata::InstallPackLoaders(std::string(g_dir) + "\\human_ai_packs\\" + pack))
            Log("aidata: pack '%s' strategies/templates can't be loaded on this game version", pack.c_str());
    }
    if (!Resolve() || !InstallHooks()) {
        Log("human_ai disabled: the game build does not match what this dll expects");
        WriteStatus("unsupported");
        LaunchApp();  // so the window can say so
        return 0;
    }
    g_resolved = true;
    // Optional: without it, air and navy simply run whenever the military minister does.
    InstallTheatreHooks(true);
    // Optional: without it, multiplayer gets no AI help (and human_ai stays off there).
    InstallMultiplayerHooks(true);
    // Optional: without it, AI packs can't be applied.
    g_defines_ready = defines::BuildRegistry();
    WriteStatus("active");
    LaunchApp();

    LogTail game_log;
    if (!GameLogPath(game_log.path)) Log("panel: Documents folder not found; in-game panel disabled");
    else Log("panel: watching %s", game_log.path);

    FILETIME last = IniWriteTime();
    for (int tick = 0;; ++tick) {
        if (game_log.path[0]) PollGameLog(game_log);
        UpdateRole();
        SyncHumanAIFlag();
        SyncScheduler();
        SyncPack(g_role);
        Sleep(250);
        if (tick % 4) continue;
        FILETIME now = IniWriteTime();
        if (CompareFileTime(&now, &last) != 0) {
            last = now;
            LoadConfig();
        }
    }
}

// Offline check, no game needed: map hoi4.exe as an image (nothing runs) and do
// every lookup the real init does, without patching. Logs to log_dir\human_ai.log.
// Returns 1 if everything resolved.
extern "C" __declspec(dllexport) int HaiSelfTest(const char* exe_path, const char* log_dir) {
    strcpy_s(g_dir, log_dir);
    Log("self-test on %s", exe_path);
    HMODULE m = LoadLibraryExA(exe_path, nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!m) {
        Log("self-test: could not map exe (error %lu)", GetLastError());
        return 0;
    }
    bool ok = scan::img.init(reinterpret_cast<uint8_t*>(m)) && Resolve() && InstallHooks(false) &&
              InstallTheatreHooks(false) && InstallMultiplayerHooks(false) && defines::BuildRegistry();
    // Optional: HAI_TEST_PACK=<folder> also parses a pack against the mapped image.
    char pack[MAX_PATH];
    if (ok && GetEnvironmentVariableA("HAI_TEST_PACK", pack, MAX_PATH)) {
        aidata::InstallPackLoaders(pack, false);
        defines::ApplyResult r = defines::ApplyPack(pack);
        Log("self-test pack: %d applied, %d not found, %d gameplay ignored, %d arrays", r.applied, r.skipped,
            r.ignored, r.tables);
        defines::Restore();
    }
    Log("self-test: %s", ok ? "PASS" : "FAIL");
    return ok ? 1 : 0;
}

}  // namespace hai
