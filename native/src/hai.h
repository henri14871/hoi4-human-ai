#pragma once
#include <windows.h>

#define HAI_VERSION "0.1.0"

namespace hai {

DWORD WINAPI InitThread(LPVOID dll_module);

void Log(const char* fmt, ...);
const char* Dir();  // folder holding hoi4.exe, this dll, human_ai.ini and human_ai.log

}  // namespace hai
