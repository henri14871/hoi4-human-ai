// AI database folders from a pack, loaded by the game's own loaders.
//
// Every database wrapper checks "DB already loaded when loading ", stores the folder,
// then reads it and finishes the database:
//   mov rdx,rdi ; mov rcx,rbx ; call <LoadFiles(db, path)>
//   mov rax,[rbx] ; mov rcx,rbx ; call [rax+0x10]        (post-process; link follows)
// We redirect that LoadFiles call: run it for the game's folder, then again for the
// same folder inside the pack, mounted into the game's file system (PhysFS) under
// "hai_pack/". The pack's entries are then post-processed and linked with the game's.
// The pack is mounted outside common/, so the game's checksummed paths don't change.
#include "aidata.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hai.h"
#include "scan.h"

extern "C" void HaiDbThunk();  // asm.asm: r10 = DbHook*, tail-jumps to HaiDbLoad

namespace aidata {

// Folders we add from a pack. Read only by the AI, so they can't desync multiplayer.
static const char* const kFolders[] = {"ai_strategy", "ai_strategy_plans", "ai_templates", "ai_equipment",
                                       "ai_areas"};

using LoadFiles_t = void(__fastcall*)(void* db, void* path);
using StrCtor_t = void*(__fastcall*)(void* str, const char* s);
using StrDtor_t = void(__fastcall*)(void* str);
using Mount_t = int (*)(const char* dir, const char* mount_point, int append);

struct DbHook {
    LoadFiles_t original;
};

static std::string g_pack_dir;
static StrCtor_t StrCtor = nullptr;
static StrDtor_t StrDtor = nullptr;
static Mount_t PhysfsMount = nullptr;
static DbHook g_hooks[16];
static int g_nhooks = 0;

// The game's string is MSVC std::string: 16-byte inline buffer, size at +0x10, capacity at +0x18.
static const char* StrData(void* s) {
    size_t cap = *reinterpret_cast<size_t*>((uint8_t*)s + 0x18);
    return cap > 15 ? *reinterpret_cast<const char**>(s) : static_cast<const char*>(s);
}

static bool PackHas(const char* folder) {
    std::string d = g_pack_dir + "\\common\\" + folder;
    DWORD a = GetFileAttributesA(d.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool MountPack() {
    static int mounted = -1;
    if (mounted >= 0) return mounted == 1;
    wchar_t wide[MAX_PATH];
    char utf8[MAX_PATH * 3];
    MultiByteToWideChar(CP_ACP, 0, g_pack_dir.c_str(), -1, wide, MAX_PATH);
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, sizeof(utf8), nullptr, nullptr);
    mounted = PhysfsMount(utf8, "hai_pack", 1) ? 1 : 0;
    hai::Log("aidata: mount %s as hai_pack/: %s", g_pack_dir.c_str(), mounted ? "ok" : "FAILED");
    return mounted == 1;
}

extern "C" void __fastcall HaiDbLoad(void* db, void* path, DbHook* hook) {
    hook->original(db, path);  // the game's own files first
    const char* p = nullptr;
    __try { p = StrData(path); } __except (EXCEPTION_EXECUTE_HANDLER) { p = nullptr; }
    if (!p || strncmp(p, "common/", 7) != 0) return;
    const char* folder = p + 7;
    bool ours = false;
    for (const char* f : kFolders) ours |= strcmp(folder, f) == 0;
    if (!ours || !PackHas(folder) || !MountPack()) return;

    char vpath[128];
    sprintf_s(vpath, "hai_pack/common/%s", folder);
    alignas(16) uint8_t str[64] = {};
    __try {
        StrCtor(str, vpath);
        hook->original(db, str);
        StrDtor(str);
        hai::Log("aidata: loaded pack folder %s", vpath);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        hai::Log("aidata: loading %s failed (exception); the game's own %s is unaffected", vpath, folder);
    }
}

static void* MakeDbStub(DbHook* h) {
    // mov r10, h ; mov rax, HaiDbThunk ; jmp rax   (stub page shared with hai.cpp's style)
    static uint8_t* page = nullptr;
    static size_t used = 0;
    if (!page) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        uintptr_t base = (uintptr_t)scan::img.base;
        for (uintptr_t d = si.dwAllocationGranularity; !page && d < 0x70000000; d += si.dwAllocationGranularity)
            if (base > d)
                page = (uint8_t*)VirtualAlloc((void*)(base - d), 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    }
    if (!page || used + 32 > 4096) return nullptr;
    uint8_t* s = page + used;
    used += 32;
    s[0] = 0x49; s[1] = 0xBA; *reinterpret_cast<void**>(s + 2) = h;
    s[10] = 0x48; s[11] = 0xB8; *reinterpret_cast<void**>(s + 12) = (void*)&HaiDbThunk;
    s[20] = 0xFF; s[21] = 0xE0;
    return s;
}

bool InstallPackLoaders(const std::string& pack_dir, bool patch) {
    using scan::img;
    g_pack_dir = pack_dir;
    int wanted = 0;
    for (const char* f : kFolders) wanted += PackHas(f) ? 1 : 0;
    if (!wanted && patch) return true;  // nothing to load: install nothing

    PhysfsMount = reinterpret_cast<Mount_t>(GetProcAddress((HMODULE)img.base, "?PHYSFS_mount@@YAHPEBD0H@Z"));

    // The loading screen loads each AI folder with
    //   lea rdx,"common/<folder>" ; lea rcx,[rsp+X] ; call <string ctor> ; nop ;
    //   lea rdx,[rsp+X] ; mov rcx,r14 ; call <db wrapper> ; nop ; lea rcx,[rsp+X] ; call <string dtor>
    // and inside each wrapper: mov rdx,rdi ; mov rcx,rbx ; call <LoadFiles> ; mov rax,[rbx] ; mov rcx,rbx ; call [rax+0x10]
    auto load_sites = scan::find(img.text,
        "48 8D 15 ?? ?? ?? ?? 48 8D 8C 24 ?? ?? ?? ?? E8 ?? ?? ?? ?? 90 48 8D 94 24 ?? ?? ?? ?? 49 8B CE E8 ?? ?? ?? ?? "
        "90 48 8D 8C 24 ?? ?? ?? ?? E8");
    std::vector<uint8_t*> sites;
    for (const char* folder : kFolders) {
        std::string want = std::string("common/") + folder;
        for (uint8_t* h : load_sites) {
            auto s = reinterpret_cast<const char*>(scan::rel32(h + 3));
            if (!img.rdata.contains(s) || want != s) continue;
            StrCtor = reinterpret_cast<StrCtor_t>(scan::rel32(h + 16));
            StrDtor = reinterpret_cast<StrDtor_t>(scan::rel32(h + 47));
            uint8_t* wrapper = scan::rel32(h + 33);
            auto c = scan::find(scan::Range{wrapper, wrapper + 0x200},
                                "48 8B D7 48 8B CB E8 ?? ?? ?? ?? 48 8B 03 48 8B CB FF 50 10");
            if (!c.empty()) sites.push_back(c[0] + 6);
        }
    }
    hai::Log("aidata: pack has %d AI folders; %zu database loaders; mount %s; string ctor %s", wanted, sites.size(),
             PhysfsMount ? "found" : "MISSING", StrCtor && StrDtor ? "found" : "MISSING");
    if (!PhysfsMount || !StrCtor || !StrDtor || sites.empty()) return false;
    if (!patch) return true;

    int patched = 0;
    for (uint8_t* call : sites) {
        if (g_nhooks >= 16) break;
        DbHook& h = g_hooks[g_nhooks];
        h.original = reinterpret_cast<LoadFiles_t>(scan::rel32(call + 1));
        void* stub = MakeDbStub(&h);
        long long rel = (uint8_t*)stub - (call + 5);
        if (!stub || rel != (int32_t)rel) continue;
        DWORD old;
        if (!VirtualProtect(call + 1, 4, PAGE_EXECUTE_READWRITE, &old)) continue;
        *reinterpret_cast<int32_t*>(call + 1) = (int32_t)rel;
        VirtualProtect(call + 1, 4, old, &old);
        FlushInstructionCache(GetCurrentProcess(), call, 5);
        ++g_nhooks;
        ++patched;
    }
    hai::Log("aidata: %d database loaders hooked", patched);
    return patched > 0;
}

}  // namespace aidata
