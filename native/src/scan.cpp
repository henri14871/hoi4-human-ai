#include "scan.h"

#include <windows.h>

#include <cstring>

namespace scan {

Image img;

bool Image::init(uint8_t* image_base) {
    base = image_base ? image_base : reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        Range r{base + sec->VirtualAddress, base + sec->VirtualAddress + sec->Misc.VirtualSize};
        if (!memcmp(sec->Name, ".text", 6)) text = r;
        else if (!memcmp(sec->Name, ".rdata", 7)) rdata = r;
        else if (!memcmp(sec->Name, ".data", 6)) data = r;
    }
    return text.begin && rdata.begin && data.begin;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::vector<uint8_t*> find(const Range& r, const char* pattern) {
    std::vector<int> pat;  // -1 = wildcard
    for (const char* p = pattern; *p;) {
        if (*p == ' ') { ++p; continue; }
        if (*p == '?') { pat.push_back(-1); p += (p[1] == '?') ? 2 : 1; continue; }
        pat.push_back(hexval(p[0]) << 4 | hexval(p[1]));
        p += 2;
    }
    std::vector<uint8_t*> hits;
    if (pat.empty() || r.end - r.begin < (ptrdiff_t)pat.size()) return hits;
    const int first = pat[0];
    for (uint8_t* p = r.begin; p + pat.size() <= r.end; ++p) {
        if (first >= 0) {
            p = static_cast<uint8_t*>(memchr(p, first, (r.end - pat.size() + 1) - p));
            if (!p) break;
        }
        size_t i = 1;
        while (i < pat.size() && (pat[i] < 0 || p[i] == pat[i])) ++i;
        if (i == pat.size()) hits.push_back(p);
    }
    return hits;
}

// MSVC x64 RTTI structures (all pointers are image-relative).
struct TypeDescriptor { void* vftable; void* spare; char name[1]; };
struct CompleteObjectLocator {
    uint32_t signature;  // 1 on x64
    uint32_t offset;     // subobject offset; 0 for the primary vtable
    uint32_t cd_offset;
    uint32_t type_descriptor;
    uint32_t class_descriptor;
    uint32_t self;
};

static uint8_t* find_bytes(const Range& r, const void* needle, size_t n, size_t align = 1) {
    const uint8_t first = *static_cast<const uint8_t*>(needle);
    for (uint8_t* p = r.begin; p + n <= r.end; ++p) {
        p = static_cast<uint8_t*>(memchr(p, first, (r.end - n + 1) - p));
        if (!p) return nullptr;
        if (((uintptr_t)p % align) == 0 && !memcmp(p, needle, n)) return p;
    }
    return nullptr;
}

void** vtable(const char* name) {
    char mangled[256];
    if (strlen(name) > 240) return nullptr;
    strcpy_s(mangled, ".?AV");
    strcat_s(mangled, name);
    strcat_s(mangled, "@@");
    // The TypeDescriptor lives in .data; its name is NUL-terminated.
    uint8_t* nm = find_bytes(img.data, mangled, strlen(mangled) + 1);
    if (!nm) return nullptr;
    const uint32_t td_rva = (uint32_t)(nm - offsetof(TypeDescriptor, name) - img.base);

    for (uint8_t* p = img.rdata.begin; p + sizeof(CompleteObjectLocator) <= img.rdata.end; p += 4) {
        auto col = reinterpret_cast<CompleteObjectLocator*>(p);
        if (col->type_descriptor != td_rva || col->signature != 1 || col->offset != 0) continue;
        if (col->self != (uint32_t)(p - img.base)) continue;
        // The vtable is preceded by a pointer to its COL.
        uint64_t col_va = (uint64_t)p;
        uint8_t* slot = find_bytes(img.rdata, &col_va, 8, 8);
        if (slot) return reinterpret_cast<void**>(slot + 8);
    }
    return nullptr;
}

const char* class_name(const void* obj) {
    __try {
        void** vt = *reinterpret_cast<void** const*>(obj);
        if (!img.rdata.contains(vt)) return nullptr;
        auto col = static_cast<CompleteObjectLocator*>(vt[-1]);
        if (!img.rdata.contains(col) || col->signature != 1) return nullptr;
        auto td = reinterpret_cast<TypeDescriptor*>(img.base + col->type_descriptor);
        if (!img.data.contains(td)) return nullptr;
        const char* n = td->name;
        return (n[0] == '.' && n[1] == '?' && n[2] == 'A') ? n + 4 : nullptr;  // skip ".?AV"
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool references_string(const uint8_t* fn, size_t len, const char* s) {
    for (size_t i = 0; i + 7 <= len; ++i) {
        if (fn[i] == 0x4C && fn[i + 1] == 0x8D && fn[i + 2] == 0x05) {  // lea r8, [rip+disp32]
            auto target = (const char*)(fn + i + 7 + *reinterpret_cast<const int32_t*>(fn + i + 3));
            if (img.rdata.contains(target) && !strcmp(target, s)) return true;
        }
    }
    return false;
}

}  // namespace scan
