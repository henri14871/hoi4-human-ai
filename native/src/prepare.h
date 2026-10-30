#pragma once
#include <string>

namespace prepare {

// Copy <pack>\common\<folder> for each folder into <pack>\.hai\common\<folder>, with the pack's
// scripted triggers expanded into the AI files. Returns the root to mount (<pack>\.hai).
std::string PreparePack(const std::string& pack_dir, const char* const* folders, int nfolders);

}  // namespace prepare
