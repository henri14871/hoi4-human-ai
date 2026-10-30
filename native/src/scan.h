// Runtime lookups inside the loaded hoi4.exe image: byte signatures and MSVC RTTI.
#pragma once
#include <cstdint>
#include <vector>

namespace scan {

struct Range {
    uint8_t* begin = nullptr;
    uint8_t* end = nullptr;
    bool contains(const void* p) const { return p >= begin && p < end; }
};

struct Image {
    uint8_t* base = nullptr;
    Range text, rdata, data;
    bool init(uint8_t* image_base = nullptr);  // default: the main executable
};

extern Image img;

// Pattern like "48 8B 0D ?? ?? ?? ?? E8". Returns every match in r.
std::vector<uint8_t*> find(const Range& r, const char* pattern);

// Target of a rip-relative disp32 that sits at p and ends an instruction at p + 4 + trail.
inline uint8_t* rel32(uint8_t* p, int trail = 0) {
    return p + 4 + trail + *reinterpret_cast<int32_t*>(p);
}

// vtable (array of function pointers) of the complete object of class `name`,
// e.g. "CAIPoliticalMinister". nullptr if not found or ambiguous.
void** vtable(const char* name);

// RTTI class name ("CCountryAI") of a polymorphic object, or nullptr.
const char* class_name(const void* obj);

// Does code in [fn, fn + len) load a pointer to the C string s via "lea r8, [rip+x]"?
bool references_string(const uint8_t* fn, size_t len, const char* s);

}  // namespace scan
