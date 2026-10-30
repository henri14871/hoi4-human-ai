// Define registry, recovered at runtime.
//
// Each define has a small reader function that loads its category table and calls a
// typed reader with the define's name and the address of its value:
//   lea rdx, "NAI" ... lea r8, [value] ; lea rdx, "NAME" ; ... call <typed reader>
// The typed readers are told apart by what they do, not by address:
//   mulss xmm0, [scale]  -> fixed point, stored as int64 round(v * scale)   (scale 100000 or 32768)
//   movss [r12], xmm0    -> float
//   mov   [r12], ebx     -> int32
// See native/README.md.
#include "defines.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "hai.h"
#include "scan.h"

namespace defines {

enum Type { kUnknown, kInt32, kFloat, kFixed };

struct Reader {
    Type type = kUnknown;
    float scale = 0;
};

struct Define {
    Type type;
    float scale;
    void* value;
};

// Only these categories are applied: they're read by AI code alone, so changing them
// on one machine can't desync a multiplayer game. Everything else is gameplay.
static const char* const kAICategories[] = {"NAI", "NAITheatre"};

static std::map<std::string, Define> g_registry;  // "NAI.NAME" -> define
static std::mutex g_mutex;

struct Saved {
    void* value;
    uint64_t bits;
    size_t size;
};
static std::vector<Saved> g_saved;

static bool IsCategory(const char* s) {
    if (!s || s[0] != 'N' || s[1] < 'A' || s[1] > 'Z' || !strcmp(s, "NDefines")) return false;
    for (const char* p = s + 1; *p; ++p)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))) return false;
    return strlen(s) < 32;
}

static bool IsDefineName(const char* s) {
    if (!s || s[0] < 'A' || s[0] > 'Z') return false;
    size_t n = 0;
    for (const char* p = s; *p; ++p, ++n)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_')) return false;
    return n >= 3 && n < 100;
}

static const char* RipString(uint8_t* disp) {
    auto s = reinterpret_cast<const char*>(scan::rel32(disp));
    return scan::img.rdata.contains(s) ? s : nullptr;
}

static Reader Classify(uint8_t* fn) {
    Reader r;
    if (!scan::img.text.contains(fn)) return r;
    scan::Range body{fn, fn + 0x300 < scan::img.text.end ? fn + 0x300 : scan::img.text.end};
    for (uint8_t* m : scan::find(body, "F3 0F 59 05")) {  // mulss xmm0, [rip+scale]
        auto* c = reinterpret_cast<float*>(scan::rel32(m + 4));
        if (scan::img.rdata.contains(c) && (*c == 100000.0f || *c == 32768.0f)) {
            r.type = kFixed;
            r.scale = *c;
            return r;
        }
    }
    if (!scan::find(body, "F3 41 0F 11 04 24").empty()) r.type = kFloat;  // movss [r12], xmm0
    else if (!scan::find(body, "41 89 1C 24").empty()) r.type = kInt32;   // mov [r12], ebx
    return r;
}

bool BuildRegistry() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_registry.clear();
    std::map<uint8_t*, Reader> readers;
    auto& text = scan::img.text;
    for (uint8_t* p : scan::find(text, "4C 8D 05 ?? ?? ?? ?? 48 8D 15")) {
        const char* name = RipString(p + 10);
        if (!IsDefineName(name)) continue;
        uint8_t* call = nullptr;
        for (int k = 14; k < 32 && p + k + 5 < text.end; ++k)
            if (p[k] == 0xE8) { call = p + k; break; }
        if (!call) continue;
        uint8_t* fn = scan::rel32(call + 1);
        auto it = readers.find(fn);
        if (it == readers.end()) it = readers.emplace(fn, Classify(fn)).first;
        // Other readers (arrays, strings) are kept as kUnknown; only arrays are ever written,
        // and only after checking the memory really is a float array of the same length.
        // Category: nearest preceding `lea rdx, "N..."` in the same small reader function.
        const char* cat = nullptr;
        for (uint8_t* q = p - 3; q > p - 0x300 && q > text.begin; --q) {
            if (q[0] == 0xCC && q[1] == 0xCC) break;  // left the function
            if (q[0] == 0x48 && q[1] == 0x8D && q[2] == 0x15) {
                const char* s = RipString(q + 3);
                if (IsCategory(s)) { cat = s; break; }
            }
        }
        if (!cat) continue;
        std::string key = std::string(cat) + "." + name;
        g_registry.emplace(key, Define{it->second.type, it->second.scale, scan::rel32(p + 3)});
    }
    size_t other = 0;
    for (auto& kv : g_registry) other += kv.second.type == kUnknown;
    hai::Log("defines: %zu registered (%zu NAI, %zu NAITheatre; %zu arrays/other)", g_registry.size(),
             RegistrySize("NAI"), RegistrySize("NAITheatre"), other);
    return RegistrySize("NAI") > 100;
}

size_t RegistrySize(const char* category) {
    std::string prefix = std::string(category) + ".";
    size_t n = 0;
    for (auto& kv : g_registry)
        if (kv.first.compare(0, prefix.size(), prefix) == 0) ++n;
    return n;
}

static void RestoreLocked() {
    for (auto it = g_saved.rbegin(); it != g_saved.rend(); ++it) memcpy(it->value, &it->bits, it->size);
    g_saved.clear();
}

void Restore() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_saved.empty()) hai::Log("defines: restored %zu game values", g_saved.size());
    RestoreLocked();
}

static void Write(const Define& d, double v) {
    Saved s{d.value, 0, d.type == kFixed ? 8u : 4u};
    memcpy(&s.bits, d.value, s.size);
    g_saved.push_back(s);
    switch (d.type) {
        case kFixed: *static_cast<int64_t*>(d.value) = (int64_t)llround(v * d.scale); break;
        case kFloat: *static_cast<float*>(d.value) = (float)v; break;
        case kInt32: *static_cast<int32_t*>(d.value) = (int32_t)llround(v); break;
        default: break;
    }
}

// Array defines are { float* data; int count; } (seen as `mov rax,[var] ; mulss xmm,[rax+4]`).
// Only overwrite when the game's array has the same length and really holds floats: an int
// array read as floats shows up as denormals, so it is refused rather than corrupted.
static bool LooksLikeFloatArray(void* value, size_t want, float** out) {
    __try {
        float* data = *static_cast<float**>(value);
        int count = *reinterpret_cast<int*>((uint8_t*)value + 8);
        if (!data || count <= 0 || count > 64 || (size_t)count != want) return false;
        for (int i = 0; i < count; ++i) {
            float x = data[i];
            if (!std::isfinite(x) || std::fabs(x) > 1e7f || (x != 0 && std::fabs(x) < 1e-6f)) return false;
        }
        *out = data;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool WriteArray(const Define& d, const std::vector<double>& vals) {
    float* data = nullptr;
    if (!LooksLikeFloatArray(d.value, vals.size(), &data)) return false;
    for (size_t i = 0; i < vals.size(); ++i) {
        Saved s{data + i, 0, 4};
        memcpy(&s.bits, data + i, 4);
        g_saved.push_back(s);
        data[i] = (float)vals[i];
    }
    return true;
}

static void StripComment(char* s) {
    if (char* c = strstr(s, "--")) *c = 0;
}

// Numbers in "{ a, b, c }"; false if anything else is in there (strings, nested tables).
static bool ParseNumbers(const std::string& body, std::vector<double>& out) {
    const char* p = body.c_str();
    while (*p) {
        if (*p == ' ' || *p == '\t' || *p == ',' || *p == '\r' || *p == '\n' || *p == '{' || *p == '}') { ++p; continue; }
        char* end = nullptr;
        double v = strtod(p, &end);
        if (end == p) return false;
        out.push_back(v);
        p = end;
    }
    return !out.empty();
}

// `NDefines.<Cat>.<NAME> = <value>` lines; tables may span lines; comments start with "--".
static void ApplyFile(const char* path, ApplyResult& r) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") || !f) return;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (strncmp(p, "NDefines.", 9) != 0) continue;
        p += 9;
        char cat[32] = {}, name[128] = {};
        size_t i = 0;
        while (*p && *p != '.' && i < sizeof(cat) - 1) cat[i++] = *p++;
        if (*p++ != '.') continue;
        i = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != '=' && i < sizeof(name) - 1) name[i++] = *p++;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p++ != '=') continue;
        while (*p == ' ' || *p == '\t') ++p;

        bool ai = false;
        for (const char* c : kAICategories) ai |= strcmp(cat, c) == 0;
        if (*p == '{') {
            // Collect the whole table, even across lines, so we never resume mid-table.
            StripComment(p);
            std::string body = p;
            int depth = 0;
            for (char ch : body) depth += ch == '{' ? 1 : ch == '}' ? -1 : 0;
            while (depth > 0 && fgets(line, sizeof(line), f)) {
                StripComment(line);
                body += line;
                for (char* q = line; *q; ++q) depth += *q == '{' ? 1 : *q == '}' ? -1 : 0;
            }
            if (!ai) { ++r.ignored; continue; }
            std::vector<double> vals;
            auto it = g_registry.find(std::string(cat) + "." + name);
            if (it == g_registry.end()) { ++r.skipped; continue; }
            if (!ParseNumbers(body, vals) || !WriteArray(it->second, vals)) { ++r.tables; continue; }
            ++r.applied;
            ++r.arrays;
            continue;
        }
        if (!ai) { ++r.ignored; continue; }
        double v;
        if (!strncmp(p, "true", 4)) v = 1;
        else if (!strncmp(p, "false", 5)) v = 0;
        else {
            char* end = nullptr;
            v = strtod(p, &end);
            if (end == p) continue;
        }
        auto it = g_registry.find(std::string(cat) + "." + name);
        if (it == g_registry.end() || it->second.type == kUnknown) { ++r.skipped; continue; }
        if (r.applied < 4) {  // a few samples so the log shows the decoding is right
            const Define& d = it->second;
            double old = d.type == kFixed ? *static_cast<int64_t*>(d.value) / (double)d.scale
                       : d.type == kFloat ? *static_cast<float*>(d.value)
                                          : *static_cast<int32_t*>(d.value);
            hai::Log("defines: %s.%s (%s) %g -> %g", cat, name,
                     d.type == kFixed ? "fixed" : d.type == kFloat ? "float" : "int", old, v);
        }
        Write(it->second, v);
        ++r.applied;
    }
    fclose(f);
}

ApplyResult ApplyPack(const std::string& pack_dir) {
    std::lock_guard<std::mutex> lock(g_mutex);
    RestoreLocked();
    ApplyResult r;
    std::string pattern = pack_dir + "\\common\\defines\\*.lua";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return r;
    std::vector<std::string> files;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    // The game loads defines files in name order; later files win.
    std::sort(files.begin(), files.end());
    for (auto& name : files) ApplyFile((pack_dir + "\\common\\defines\\" + name).c_str(), r);
    return r;
}

}  // namespace defines
