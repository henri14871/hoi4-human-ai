#pragma once
#include <string>

namespace prepare {

// Copy <pack>\common\<folder> for each folder into <pack>\.hai\common\<folder>, with the pack's
// scripted triggers expanded into the AI files. Returns the root to mount (<pack>\.hai).
// scoped: also make every top-level entry require `is_ai = no` in its `allowed` block, so AI
// nations never use the pack's entries (multiplayer-safe mode; see aidata.h).
std::string PreparePack(const std::string& pack_dir, const char* const* folders, int nfolders, bool scoped = false);

}  // namespace prepare
