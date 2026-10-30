// ImGui overlay for the Human AI panel.
// Hooks IDXGISwapChain::Present via inline detour to render on top of HoI4.
#include "overlay.h"
#include "hai.h"

#include <atomic>
#include <cstring>
#include <d3d11.h>
#include <dxgi.h>

#include "../imgui/imgui.h"
#include "../imgui/backends/imgui_impl_dx11.h"
#include "../imgui/backends/imgui_impl_win32.h"

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace hai {

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static std::atomic<bool> g_show{false};
static bool g_imgui_ready = false;

static ID3D11Device*           g_device = nullptr;
static ID3D11DeviceContext*    g_context = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static HWND                    g_hwnd = nullptr;
static WNDPROC                 g_orig_wndproc = nullptr;

// ---------------------------------------------------------------------------
// Trampoline: 14 bytes of saved prologue + 14-byte absolute jump to original+14.
// Allocated as PAGE_EXECUTE_READWRITE so we can call through it.
// ---------------------------------------------------------------------------
static uint8_t* g_trampoline = nullptr;
static uint8_t* g_present_target = nullptr;
constexpr int kDetourSize = 14;  // FF 25 [00000000] [addr64]

using PresentFn = HRESULT(WINAPI*)(IDXGISwapChain*, UINT, UINT);

static PresentFn OriginalPresent() {
    return reinterpret_cast<PresentFn>(g_trampoline);
}

// ---------------------------------------------------------------------------
// WndProc hook
// ---------------------------------------------------------------------------
static LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && (int)wp == MenuKey()) {
        g_show = !g_show;
        return 0;
    }
    if (g_show && g_imgui_ready) {
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
        if (ImGui::GetIO().WantCaptureMouse || ImGui::GetIO().WantCaptureKeyboard)
            return 0;
    }
    return CallWindowProcW(g_orig_wndproc, hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Panel UI
// ---------------------------------------------------------------------------
static const char* const kGroupLabels[] = {"Government", "Armed forces", "Intelligence"};
struct GroupRange { int begin; int end; };
static const GroupRange kGroups[] = {{0, 3}, {3, 6}, {6, 8}};

static const char* PrettyName(const char* key) {
    if (!strcmp(key, "foreign"))    return "Diplomacy";
    if (!strcmp(key, "interior"))   return "Economy";
    if (!strcmp(key, "political"))  return "Politics";
    if (!strcmp(key, "land"))       return "Land forces";
    if (!strcmp(key, "air"))        return "Air force";
    if (!strcmp(key, "navy"))       return "Navy";
    if (!strcmp(key, "volunteers")) return "Volunteers";
    if (!strcmp(key, "cryptology")) return "Cryptology";
    return key;
}

static void DrawPanel() {
    ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Human AI", &g_show, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    Status st = GetStatus();
    if (!st.resolved) {
        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "Engine not resolved for this game build.");
        ImGui::End();
        return;
    }
    if (st.chain_bad) {
        ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "Module chain failed; standing down.");
        ImGui::End();
        return;
    }

    bool enabled = Enabled();
    if (ImGui::Checkbox("Enabled", &enabled))
        SetEnabled(enabled);

    ImGui::Separator();

    int n = CategoryCount();
    for (int g = 0; g < 3; ++g) {
        ImGui::TextDisabled("%s", kGroupLabels[g]);
        for (int c = kGroups[g].begin; c < kGroups[g].end && c < n; ++c) {
            bool ai = AIControls(c);
            if (ImGui::Checkbox(PrettyName(CategoryKey(c)), &ai))
                SetAIControls(c, ai);
        }
        if (g < 2) ImGui::Spacing();
    }

    ImGui::Separator();
    if (ImGui::Button("All to AI")) {
        for (int c = 0; c < n; ++c) SetAIControls(c, true);
    }
    ImGui::SameLine();
    if (ImGui::Button("All to me")) {
        for (int c = 0; c < n; ++c) SetAIControls(c, false);
    }

    ImGui::Separator();
    ImGui::TextDisabled("Hooks: %d modules, %d sites | Flag: %s",
                        st.module_hooks, st.site_hooks, st.engine_flag ? "ON" : "off");

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Present detour
// ---------------------------------------------------------------------------
static void EnsureRTV(IDXGISwapChain* sc) {
    if (g_rtv) return;
    ID3D11Texture2D* back = nullptr;
    sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back);
    if (back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

static HRESULT WINAPI HookedPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!g_imgui_ready) {
        sc->GetDevice(__uuidof(ID3D11Device), (void**)&g_device);
        if (g_device) {
            g_device->GetImmediateContext(&g_context);
            DXGI_SWAP_CHAIN_DESC desc{};
            sc->GetDesc(&desc);
            g_hwnd = desc.OutputWindow;

            ImGui::CreateContext();
            ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            ImGui::StyleColorsDark();
            ImGui_ImplWin32_Init(g_hwnd);
            ImGui_ImplDX11_Init(g_device, g_context);

            g_orig_wndproc = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)HookedWndProc);
            g_imgui_ready = true;
            Log("overlay: ImGui initialized (hwnd=%p)", g_hwnd);
        }
    }

    if (g_imgui_ready && g_show) {
        EnsureRTV(sc);
        if (g_rtv) {
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            DrawPanel();
            ImGui::Render();
            g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }
    }

    return OriginalPresent()(sc, sync, flags);
}

// ---------------------------------------------------------------------------
// Hook installation
// ---------------------------------------------------------------------------
static void WriteAbsJump(uint8_t* at, void* target) {
    at[0] = 0xFF; at[1] = 0x25;
    *reinterpret_cast<uint32_t*>(at + 2) = 0;
    *reinterpret_cast<void**>(at + 6) = target;
}

static bool InstallPresentHook() {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"HaiDummy";
    wc.hInstance = GetModuleHandleW(nullptr);
    RegisterClassExW(&wc);
    HWND dummy = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
                                 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
    if (!dummy) {
        Log("overlay: dummy window creation failed");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = dummy;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, nullptr);
    if (FAILED(hr)) {
        DestroyWindow(dummy);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        Log("overlay: D3D11CreateDeviceAndSwapChain failed (%#lx)", hr);
        return false;
    }

    void** vt = *reinterpret_cast<void***>(sc);
    g_present_target = reinterpret_cast<uint8_t*>(vt[8]);

    // Build trampoline: original prologue bytes + absolute jump to original+14.
    g_trampoline = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_trampoline) {
        sc->Release();
        dev->Release();
        DestroyWindow(dummy);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        Log("overlay: trampoline alloc failed");
        return false;
    }
    memcpy(g_trampoline, g_present_target, kDetourSize);
    WriteAbsJump(g_trampoline + kDetourSize, g_present_target + kDetourSize);

    // Patch the original function's entry point.
    DWORD old;
    VirtualProtect(g_present_target, kDetourSize, PAGE_EXECUTE_READWRITE, &old);
    WriteAbsJump(g_present_target, (void*)&HookedPresent);
    FlushInstructionCache(GetCurrentProcess(), g_present_target, kDetourSize);
    VirtualProtect(g_present_target, kDetourSize, old, &old);

    sc->Release();
    dev->Release();
    DestroyWindow(dummy);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    Log("overlay: Present detoured at %p, trampoline at %p", g_present_target, g_trampoline);
    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void OverlayInit() {
    if (!InstallPresentHook())
        Log("overlay: failed to hook Present; panel unavailable (use human_ai.ini)");
}

void OverlayTick() {
    if (g_rtv && !g_show) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}

}  // namespace hai
