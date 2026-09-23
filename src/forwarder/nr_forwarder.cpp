// The four DLSSNR calls the snippet gates on its caller, isolated in a module it will accept.
//
// nvngx_dlssnr.dll resolves the module owning its return address and requires that module's path to
// contain "nvngx.dll" (the driver core is _nvngx.dll); everything else gets FAIL_PlatformError before
// a single argument is read. So this DLL is named nvngx.dll_nrfwd.dll and does nothing but load the
// snippet and forward Init/Create/Evaluate/Release. All parameter-block writes stay in the addon --
// the block's setters are the driver's own and check nothing.
//
// Every snippet result is assigned to a volatile rather than returned directly: `return f(...)` is a
// tail call, the compiler emits a jmp, and the snippet would resolve its caller past this module --
// which the gate rejects. (Learned from OptiScaler_DLSSNR's forwarder, MIT.)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d12.h>

namespace {

using PFN_InitExt = int(__cdecl*)(unsigned long long, const wchar_t*, ID3D12Device*, int, const void*);
using PFN_Create = int(__cdecl*)(ID3D12GraphicsCommandList*, int, const void*, void**);
using PFN_Evaluate = int(__cdecl*)(ID3D12GraphicsCommandList*, const void*, const void*, void*);
using PFN_Release = int(__cdecl*)(void*);
using PFN_Shutdown1 = int(__cdecl*)(ID3D12Device*);

struct Snippet {
  HMODULE module = nullptr;
  PFN_InitExt init = nullptr;
  PFN_Create create = nullptr;
  PFN_Evaluate evaluate = nullptr;
  PFN_Release release = nullptr;
  PFN_Shutdown1 shutdown = nullptr;
  bool initialised = false;
};

Snippet g_snip;

bool LoadSnippet(const wchar_t* path) {
  if (g_snip.module != nullptr) return g_snip.create != nullptr;
  g_snip.module = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (g_snip.module == nullptr) return false;
  g_snip.init = (PFN_InitExt)GetProcAddress(g_snip.module, "NVSDK_NGX_D3D12_Init_Ext");
  g_snip.create = (PFN_Create)GetProcAddress(g_snip.module, "NVSDK_NGX_D3D12_CreateFeature");
  g_snip.evaluate = (PFN_Evaluate)GetProcAddress(g_snip.module, "NVSDK_NGX_D3D12_EvaluateFeature");
  g_snip.release = (PFN_Release)GetProcAddress(g_snip.module, "NVSDK_NGX_D3D12_ReleaseFeature");
  g_snip.shutdown = (PFN_Shutdown1)GetProcAddress(g_snip.module, "NVSDK_NGX_D3D12_Shutdown1");
  return g_snip.create != nullptr && g_snip.evaluate != nullptr;
}

}  // namespace

extern "C" {

// Loads the snippet and initialises its NGX core: generic application id, SDK 0x15 (what the working
// path passes -- the feature postdates the SDK versions games declare), and the driver core's
// capability block as the feature-info argument. Returns the snippet's init result (1 = success),
// 0 if the snippet would not load, or the negative of the missing piece.
__declspec(dllexport) int nrfwd_init(const wchar_t* snippet_path, const wchar_t* data_path,
                                     ID3D12Device* device, void* capability_params) {
  if (!LoadSnippet(snippet_path)) return 0;
  if (g_snip.initialised) return 1;
  if (g_snip.init == nullptr) return -1;
  volatile int result = g_snip.init(0x24480451ull, data_path, device, 0x0000015, capability_params);
  g_snip.initialised = (result == 1);
  return result;
}

// Creates feature 18. The addon has already written every DLSSNR.* creation parameter into the
// block. Initialisation work is recorded into cmd, so the handle must outlive that list's execution.
__declspec(dllexport) void* nrfwd_create(ID3D12GraphicsCommandList* cmd, void* capability_params,
                                         int* out_result) {
  if (g_snip.create == nullptr || capability_params == nullptr) {
    if (out_result != nullptr) *out_result = 0;
    return nullptr;
  }
  void* handle = nullptr;
  volatile int result = g_snip.create(cmd, 18, capability_params, &handle);
  if (out_result != nullptr) *out_result = result;
  return result == 1 ? handle : nullptr;
}

__declspec(dllexport) int nrfwd_evaluate(ID3D12GraphicsCommandList* cmd, void* feature,
                                         void* capability_params) {
  if (g_snip.evaluate == nullptr || feature == nullptr) return 0;
  volatile int result = g_snip.evaluate(cmd, feature, capability_params, nullptr);
  return result;
}

__declspec(dllexport) void nrfwd_release(void* feature) {
  if (g_snip.release != nullptr && feature != nullptr) {
    volatile int ignored = g_snip.release(feature);
    (void)ignored;
  }
}

// Shuts the snippet's NGX core down for one device, so the next nrfwd_init initialises it again
// -- against a new device, or against the driver core's new capability block after the game
// restarted NGX. Without this, nrfwd_init answered "already initialised" for a core bound to a
// device and block that no longer existed. Shutdown1 carries the same caller gate as the rest.
// Returns the snippet's result (1 = success), or 0 when there was nothing to shut down.
__declspec(dllexport) int nrfwd_shutdown(ID3D12Device* device) {
  if (!g_snip.initialised) return 0;
  g_snip.initialised = false;
  if (g_snip.shutdown == nullptr) return 0;
  volatile int result = g_snip.shutdown(device);
  return result;
}

}  // extern "C"
