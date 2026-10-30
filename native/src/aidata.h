// AI packs, part 2: load a pack's AI database folders (ai_strategy, ai_templates, ...)
// into the game's own databases at startup, alongside the game's files.
#pragma once
#include <string>

namespace aidata {

// Hook the AI database loaders so they also read <pack_dir>\common\<folder>.
// Must run before the loading screen reaches the AI databases. Does nothing (and
// installs nothing) if the pack has none of the supported folders.
// With patch=false only the lookups run (self-test).
bool InstallPackLoaders(const std::string& pack_dir, bool patch = true);

// True once any pack folder was loaded into the game's databases this session. AI strategies are
// evaluated on every machine (their values are in the synced checksum), so this desyncs multiplayer.
bool LoadedAny();

}  // namespace aidata
