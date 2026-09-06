/* ---------------------------------------------------------------------------
 * slgi_forwarder.c -- the shim slang-gfx loads instead of d3d12 / dxgi.
 *
 * Built once and installed under BOTH names the gfx.dll patch introduces:
 * slp12.dll (where slang-gfx asked for "d3d12") and slgi.dll (where it asked
 * for "dxgi"). Every entry point simply calls through to sl.interposer.dll.
 *
 * ---------------------------------------------------------------------------
 * WHY THUNKS AND NOT EXPORT FORWARDING.
 *
 * The tidy version of this file is a dozen /export:Name=Target.Name pragmas and
 * no code at all. It cannot work here, for a reason that is entirely
 * non-obvious: the Windows loader splits a forward string at the FIRST dot, so
 *
 *     sl.interposer.CreateDXGIFactory2
 *
 * is read as module "sl", function "interposer.CreateDXGIFactory2". A module
 * name containing dots can never be a forward target. The failure is silent in
 * the worst way -- the DLL loads, and GetProcAddress just returns null, so
 * slang-gfx reports "failed load symbol 'CreateDXGIFactory'" and then "Did not
 * find any GPUs for device type 'D3D12'", neither of which points anywhere near
 * the actual cause.
 *
 * Thunks have no such restriction: LoadLibrary takes the name whole.
 *
 * ---------------------------------------------------------------------------
 * WHY THE INTERPOSER MUST KEEP ITS OWN NAME, which is what forces all this.
 *
 * Two earlier shapes of this shim failed:
 *
 *   COPIES of sl.interposer.dll under the two new names. Windows keys modules
 *   by PATH, so that is two interposers with separate state; slInit reaches one
 *   and the other sits half-initialised under the DXGI factory. The process
 *   segfaults during swapchain creation, before the engine prints a line.
 *
 *   RENAMING the single interposer to slp12.dll. Streamline finds its own
 *   plugins and OTA cache from its module path, and it locates that path by
 *   looking for a module called sl.interposer.dll. Renamed it cannot --
 *   "ota.cpp: Unable to determine SL Interposer DLL path" -- and its plugins
 *   then load twice and are discarded as duplicates.
 *
 * So: exactly one interposer, loaded under its real name so it can still find
 * itself, with this stateless shim in front of it. Being stateless is what
 * makes it safe to install the same file under both names.
 *
 * The entry points below are the union of what slang-gfx resolves from each
 * module (tools/gfx/d3d/d3d-util.cpp and d3d12/d3d12-device.cpp), and each was
 * checked against sl.interposer.dll's real export table -- a thunk to a name
 * the interposer does not export would return null and reproduce exactly the
 * failure this file exists to fix.
 * ------------------------------------------------------------------------- */
#include <windows.h>

static HMODULE sl_module(void)
{
    /* Resolved once. LoadLibrary refcounts by path, so this is the same module
     * instance slInit runs on -- which is the whole point. */
    static HMODULE s_module = NULL;
    if (!s_module)
        s_module = LoadLibraryA("sl.interposer.dll");
    return s_module;
}

static FARPROC sl_proc(const char *name)
{
    HMODULE m = sl_module();
    return m ? GetProcAddress(m, name) : NULL;
}

/* One thunk per entry point. The signatures have to match exactly -- these are
 * called by slang-gfx through a function pointer it obtained from us. */

#define SL_THUNK(ret, name, params, args, fail)                      \
    __declspec(dllexport) ret WINAPI name params                     \
    {                                                                \
        typedef ret(WINAPI * fn_t) params;                           \
        static fn_t s_fn = NULL;                                     \
        if (!s_fn) s_fn = (fn_t)sl_proc(#name);                      \
        if (!s_fn) return fail;                                      \
        return s_fn args;                                            \
    }

/* The parameter types are deliberately generic. These functions only pass their
 * arguments straight through, so what matters is the ABI -- pointer-sized and
 * int-sized -- not the declared type. Writing them as void* and UINT keeps this
 * a plain C file with no dependency on d3d12.h or dxgi.h, whose real types
 * (IUnknown*, REFIID, D3D_FEATURE_LEVEL) are all exactly that width anyway. */

/* --- D3D12 ---------------------------------------------------------------- */

/* D3D12CreateDevice is NOT a plain thunk, and that is the whole reason frame
 * generation spent so long silently doing nothing.
 *
 * Streamline will not initialise its plugins until it has been handed the
 * device, and its plugin manager says so in as many words:
 *
 *     "Plugins already initialized but could be using the wrong device,
 *      please call slSetD3DDevice immediately after creating desired device"
 *
 * "Immediately" is load-bearing. DLSS-G does its real work from a hook on
 * IDXGIFactory::CreateSwapChainForHwnd -- that is where it wraps the swap
 * chain so it can insert frames into it later. Hooks only run once the plugins
 * are initialised, and the plugins only initialise once the device is known.
 *
 * Falcor creates the device in SampleApp.cpp:66 and the swap chain twenty
 * lines later at :86, while the earliest point an application can act is
 * onLoad(), at :284. Calling slSetD3DDevice from there is about 600 ms and one
 * swap chain too late: DLSS-G has already missed the only creation it cared
 * about. Its Present hook still runs afterwards, finds no swap chain of its
 * own, and passes every frame straight through -- no error, no warning, no
 * generated frames, which is exactly what makes this so hard to see. In the
 * log it surfaces only as four lines of
 *
 *     "D3D or VK API hook is activated without device being created"
 *
 * landing in the same microsecond as "Upgraded IDXGISwapChain v0 to v2".
 *
 * This shim is the one piece of code that sits between those two events, so
 * this is where the device has to be handed over. */
__declspec(dllexport) HRESULT WINAPI D3D12CreateDevice(void *a, UINT b, const void *c, void **d)
{
    typedef HRESULT(WINAPI * create_fn)(void *, UINT, const void *, void **);
    typedef int(*set_device_fn)(void *); /* sl::Result; eOk == 0 */

    static create_fn s_create = NULL;
    static int s_handedOver = 0;
    HRESULT hr;

    if (!s_create) s_create = (create_fn)sl_proc("D3D12CreateDevice");
    if (!s_create) return E_FAIL;

    hr = s_create(a, b, c, d);

    /* d is null when the caller is only probing feature-level support, which
     * slang-gfx does before it commits to an adapter. Nothing was created in
     * that case, so there is nothing to hand over. */
    if (SUCCEEDED(hr) && d && *d && !s_handedOver)
    {
        set_device_fn set = (set_device_fn)sl_proc("slSetD3DDevice");
        /* Latch only on success. If slInit somehow has not run yet this fails
         * harmlessly and the next device creation tries again. */
        if (set && set(*d) == 0) s_handedOver = 1;
    }
    return hr;
}
SL_THUNK(HRESULT, D3D12GetDebugInterface, (const void *a, void **b), (a, b), E_FAIL)
SL_THUNK(HRESULT, D3D12GetInterface, (const void *a, const void *b, void **c), (a, b, c), E_FAIL)
SL_THUNK(HRESULT, D3D12SerializeRootSignature,
         (const void *a, UINT b, void **c, void **d), (a, b, c, d), E_FAIL)
SL_THUNK(HRESULT, D3D12SerializeVersionedRootSignature,
         (const void *a, void **b, void **c), (a, b, c), E_FAIL)
SL_THUNK(HRESULT, D3D12CreateRootSignatureDeserializer,
         (const void *a, SIZE_T b, const void *c, void **d), (a, b, c, d), E_FAIL)
SL_THUNK(HRESULT, D3D12CreateVersionedRootSignatureDeserializer,
         (const void *a, SIZE_T b, const void *c, void **d), (a, b, c, d), E_FAIL)
SL_THUNK(HRESULT, D3D12EnableExperimentalFeatures,
         (UINT a, const void *b, void *c, UINT *d), (a, b, c, d), E_FAIL)

/* --- DXGI ----------------------------------------------------------------- */
SL_THUNK(HRESULT, CreateDXGIFactory, (const void *a, void **b), (a, b), E_FAIL)
SL_THUNK(HRESULT, CreateDXGIFactory1, (const void *a, void **b), (a, b), E_FAIL)
SL_THUNK(HRESULT, CreateDXGIFactory2, (UINT a, const void *b, void **c), (a, b, c), E_FAIL)
SL_THUNK(HRESULT, DXGIGetDebugInterface1, (UINT a, const void *b, void **c), (a, b, c), E_FAIL)

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst;
    (void)reason;
    (void)reserved;
    return TRUE;
}
