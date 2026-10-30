#pragma once
#include <windows.h>

#define HAI_VERSION "0.1.0"

namespace hai {

DWORD WINAPI InitThread(LPVOID dll_module);

void Log(const char* fmt, ...);
const char* Dir();  // folder holding hoi4.exe, this dll, human_ai.ini and human_ai.log

// Settings, shared with the overlay. Setters also save human_ai.ini.
int CategoryCount();
const char* CategoryKey(int c);
bool AIControls(int c);
void SetAIControls(int c, bool ai);
bool Enabled();
void SetEnabled(bool on);
int MenuKey();  // virtual-key code that toggles the overlay

struct Status {
    bool resolved;      // engine functions found for this game build
    int module_hooks;   // AI module update slots patched (expected 56)
    int site_hooks;     // air/navy call sites patched (expected 6)
    bool engine_flag;   // the engine's human_ai switch is on
    bool chain_bad;     // module -> country lookup failed, stood down
};
Status GetStatus();

}  // namespace hai
