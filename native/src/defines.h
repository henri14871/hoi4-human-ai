// AI packs: apply the AI-only defines (NAI, NAITheatre) from an AI mod's lua files
// in memory, without loading the mod (so the multiplayer checksum is untouched).
#pragma once
#include <string>

namespace defines {

// Find every define the game registers (category, name, type, address). Call once.
bool BuildRegistry();
size_t RegistrySize(const char* category);

struct ApplyResult {
    int applied = 0;   // values written
    int skipped = 0;   // AI defines the game doesn't have (stale or renamed)
    int ignored = 0;   // non-AI defines (gameplay), never applied
    int tables = 0;    // array values, not supported yet
};

// Apply pack_dir\common\defines\*.lua. Restores any previous pack first.
ApplyResult ApplyPack(const std::string& pack_dir);
// Put back the game's own values.
void Restore();

}  // namespace defines
