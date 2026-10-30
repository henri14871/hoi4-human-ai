// version.dll proxy: forwards every export to the real System32 copy, and on
// load inside hoi4.exe starts the human_ai init thread.
#include <windows.h>

#include "hai.h"

extern "C" void* g_ver[17] = {};

static const char* const kVersionExports[17] = {
    "GetFileVersionInfoA",     "GetFileVersionInfoByHandle", "GetFileVersionInfoExA",
    "GetFileVersionInfoExW",   "GetFileVersionInfoSizeA",    "GetFileVersionInfoSizeExA",
    "GetFileVersionInfoSizeExW", "GetFileVersionInfoSizeW",  "GetFileVersionInfoW",
    "VerFindFileA",            "VerFindFileW",               "VerInstallFileA",
    "VerInstallFileW",         "VerLanguageNameA",           "VerLanguageNameW",
    "VerQueryValueA",          "VerQueryValueW",
};

static bool LoadRealVersionDll() {
    char path[MAX_PATH];
    UINT n = GetSystemDirectoryA(path, MAX_PATH);
    if (n == 0 || n > MAX_PATH - 16) return false;
    lstrcatA(path, "\\version.dll");
    HMODULE real = LoadLibraryA(path);
    if (!real) return false;
    for (int i = 0; i < 17; ++i) g_ver[i] = (void*)GetProcAddress(real, kVersionExports[i]);
    return true;
}

static bool IsHoi4Process() {
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (n == 0) return false;
    const char* name = exe + n;
    while (name > exe && name[-1] != '\\' && name[-1] != '/') --name;
    return lstrcmpiA(name, "hoi4.exe") == 0;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        if (!LoadRealVersionDll()) return FALSE;
        // The launcher and dowser.exe live in the same folder and would load us too.
        if (IsHoi4Process()) {
            HANDLE t = CreateThread(nullptr, 0, hai::InitThread, self, 0, nullptr);
            if (t) CloseHandle(t);
        }
    }
    return TRUE;
}
