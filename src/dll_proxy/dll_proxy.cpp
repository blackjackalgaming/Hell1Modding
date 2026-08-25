#include "dll_proxy.hpp"

// D3D11 proxy, adapted from Hell2Modding's D3D12 version.
//
// How it works: Windows loads our d3d11.dll from the game folder instead of
// the system one. We load the real d3d11.dll from System32 ourselves, cache a
// pointer to each of its exports, and expose stubs under the same names. Each
// stub parks the original address in PA and jumps to it via runASM (see
// d3d11_proxy.asm), so the call continues on to the real implementation.
//
// IMPORTANT: build Release, not Debug. The stubs only work because the
// compiler tail-call optimises `PA = ...; runASM();` into a bare jump, leaving
// the stack exactly as the caller set it up. Without that optimisation the
// prologue shifts RSP and any export taking more than four arguments (e.g.
// D3D11CreateDeviceAndSwapChain, which takes eleven) reads garbage. Debug
// builds are also ruled out separately by the eastl::string ABI assert.

#define D3D11_EXPORTS(X)                        \
    X(CreateDirect3D11DeviceFromDXGIDevice)     \
    X(CreateDirect3D11SurfaceFromDXGISurface)   \
    X(D3D11CoreCreateDevice)                    \
    X(D3D11CoreCreateLayeredDevice)             \
    X(D3D11CoreGetLayeredDeviceSize)            \
    X(D3D11CoreRegisterLayers)                  \
    X(D3D11CreateDevice)                        \
    X(D3D11CreateDeviceAndSwapChain)            \
    X(D3D11CreateDeviceForD3D12)                \
    X(D3D11On12CreateDevice)                    \
    X(D3DKMTCloseAdapter)                       \
    X(D3DKMTCreateAllocation)                   \
    X(D3DKMTCreateContext)                      \
    X(D3DKMTCreateDevice)                       \
    X(D3DKMTCreateSynchronizationObject)        \
    X(D3DKMTDestroyAllocation)                  \
    X(D3DKMTDestroyContext)                     \
    X(D3DKMTDestroyDevice)                      \
    X(D3DKMTDestroySynchronizationObject)       \
    X(D3DKMTEscape)                             \
    X(D3DKMTGetContextSchedulingPriority)       \
    X(D3DKMTGetDeviceState)                     \
    X(D3DKMTGetDisplayModeList)                 \
    X(D3DKMTGetMultisampleMethodList)           \
    X(D3DKMTGetRuntimeData)                     \
    X(D3DKMTGetSharedPrimaryHandle)             \
    X(D3DKMTLock)                               \
    X(D3DKMTOpenAdapterFromHdc)                 \
    X(D3DKMTOpenResource)                       \
    X(D3DKMTPresent)                            \
    X(D3DKMTQueryAdapterInfo)                   \
    X(D3DKMTQueryAllocationResidency)           \
    X(D3DKMTQueryResourceInfo)                  \
    X(D3DKMTRender)                             \
    X(D3DKMTSetAllocationPriority)              \
    X(D3DKMTSetContextSchedulingPriority)       \
    X(D3DKMTSetDisplayMode)                     \
    X(D3DKMTSetDisplayPrivateDriverFormat)      \
    X(D3DKMTSetGammaRamp)                       \
    X(D3DKMTSetVidPnSourceOwner)                \
    X(D3DKMTSignalSynchronizationObject)        \
    X(D3DKMTUnlock)                             \
    X(D3DKMTWaitForSynchronizationObject)       \
    X(D3DKMTWaitForVerticalBlankEvent)          \
    X(D3DPerformance_BeginEvent)                \
    X(D3DPerformance_EndEvent)                  \
    X(D3DPerformance_GetStatus)                 \
    X(D3DPerformance_SetMarker)                 \
    X(EnableFeatureLevelUpgrade)                \
    X(OpenAdapter10)                            \
    X(OpenAdapter10_2)

// Cached handles to the real d3d11.dll and each of its exports.
#define DECLARE_ORIGINAL(name) FARPROC o##name;

struct D3D11_dll
{
    HMODULE dll;
    D3D11_EXPORTS(DECLARE_ORIGINAL)
} D3D11;

#undef DECLARE_ORIGINAL

extern "C"
{
    FARPROC PA = 0;
    int runASM();

// One stub per export. The names here (fD3D11CreateDevice, etc.) are what
// d3d11_proxy.def maps the real export names onto.
#define DEFINE_STUB(name)     \
    void f##name()            \
    {                         \
        PA = D3D11.o##name;   \
        runASM();             \
    }

    D3D11_EXPORTS(DEFINE_STUB)

#undef DEFINE_STUB
}

#define LOAD_ORIGINAL(name) D3D11.o##name = GetProcAddress(D3D11.dll, #name);

static void setupFunctions()
{
    if (!D3D11.dll)
    {
        return;
    }

    D3D11_EXPORTS(LOAD_ORIGINAL)
}

#undef LOAD_ORIGINAL

namespace big
{
    void dll_proxy::init()
    {
        BOOL wow64 = FALSE;
        WCHAR path[MAX_PATH];

        if (IsWow64Process(GetCurrentProcess(), &wow64) && wow64)
        {
            GetSystemWow64DirectoryW(path, MAX_PATH);
        }
        else
        {
            GetSystemDirectoryW(path, MAX_PATH);
        }

        lstrcatW(path, L"\\");
        lstrcatW(path, L"D3D11.dll");
        D3D11.dll = LoadLibraryW(path);

        setupFunctions();
    }

    void* dll_proxy::original_module()
    {
        return D3D11.dll;
    }
} // namespace big
