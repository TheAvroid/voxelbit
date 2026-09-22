#pragma once
// ---------------------------------------------------------------------------
// dred.h -- WHAT THE GPU WAS DOING WHEN THE DEVICE WENT.
//
// (user 2026-09-21: "when killing a peice of life my game crashes with this",
//  and a screenshot of DXGI_ERROR_DEVICE_REMOVED.)
//
// ---------------------------------------------------------------------------
// WHY A SECOND CRASH REPORTER
// ---------------------------------------------------------------------------
//
// platform/crashlog.h catches a CPU fault and gives it a stack. A removed
// device is not a CPU fault: the GPU faults, the driver tears the device down,
// and the next API call on the CPU fails with DXGI_ERROR_DEVICE_REMOVED -- so
// the stack points at whatever innocent call happened to be next. The report
// that started this names `Falcor::Gui::render` mapping a constant buffer,
// which had nothing to do with it.
//
// DRED is the GPU's own black box, and it answers the two questions a stack
// cannot:
//
//   BREADCRUMBS   the last operations the command list actually completed,
//                 in order. The one after the last completed node is the one
//                 that killed it.
//   PAGE FAULT    the virtual address that was read or written illegally, and
//                 -- the useful part -- the names and lifetimes of the
//                 resources that were allocated at that address. "Recently
//                 freed: v2::blasScratch" is the whole diagnosis of an
//                 aliasing bug.
//
// ---------------------------------------------------------------------------
// IT MUST BE TURNED ON BEFORE THE DEVICE EXISTS
// ---------------------------------------------------------------------------
//
// D3D12GetDebugInterface hands out a settings object that only affects devices
// created AFTER it, so enable() is called from main() alongside
// installCrashHandler and before Falcor builds anything. Miss that ordering and
// every field below reads back empty, which looks exactly like "DRED found
// nothing".
//
// COST: breadcrumbs make the driver write a marker per operation. NVIDIA's
// guidance is that this is measurable but small, and it is off unless asked
// for -- V2_DRED=1, or --debug, which nobody has on while playing.
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <d3d12.h>
#endif

namespace v2 {

#ifdef _WIN32
namespace detail {

inline bool &dredOn() {
    static bool on = false;
    return on;
}

// The device, remembered so the crash path can ask it questions. A raw
// pointer and not a ref: this is read while the device is already dead and
// must not extend its life or take a lock.
inline ID3D12Device *&dredDevice() {
    static ID3D12Device *d = nullptr;
    return d;
}

inline const char *opName(D3D12_AUTO_BREADCRUMB_OP op) {
    switch (op) {
        case D3D12_AUTO_BREADCRUMB_OP_SETMARKER:            return "SetMarker";
        case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT:           return "BeginEvent";
        case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT:             return "EndEvent";
        case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED:        return "DrawInstanced";
        case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return "DrawIndexedInstanced";
        case D3D12_AUTO_BREADCRUMB_OP_DISPATCH:             return "Dispatch";
        case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION:     return "CopyBufferRegion";
        case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE:         return "CopyResource";
        case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER:      return "ResourceBarrier";
        case D3D12_AUTO_BREADCRUMB_OP_BUILDRAYTRACINGACCELERATIONSTRUCTURE:
            return "BuildRaytracingAccelerationStructure";
        case D3D12_AUTO_BREADCRUMB_OP_COPYRAYTRACINGACCELERATIONSTRUCTURE:
            return "CopyRaytracingAccelerationStructure";
        case D3D12_AUTO_BREADCRUMB_OP_EMITRAYTRACINGACCELERATIONSTRUCTUREPOSTBUILDINFO:
            return "EmitRaytracingAccelerationStructurePostbuildInfo";
        case D3D12_AUTO_BREADCRUMB_OP_DISPATCHRAYS:         return "DispatchRays";
        case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT:      return "ExecuteIndirect";
        case D3D12_AUTO_BREADCRUMB_OP_PRESENT:              return "Present";
        default:                                            return "op";
    }
}

inline const char *allocName(D3D12_DRED_ALLOCATION_TYPE t) {
    switch (t) {
        case D3D12_DRED_ALLOCATION_TYPE_COMMAND_QUEUE:     return "command queue";
        case D3D12_DRED_ALLOCATION_TYPE_COMMAND_ALLOCATOR: return "command allocator";
        case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_STATE:    return "pipeline state";
        case D3D12_DRED_ALLOCATION_TYPE_COMMAND_LIST:      return "command list";
        case D3D12_DRED_ALLOCATION_TYPE_FENCE:             return "fence";
        case D3D12_DRED_ALLOCATION_TYPE_DESCRIPTOR_HEAP:   return "descriptor heap";
        case D3D12_DRED_ALLOCATION_TYPE_HEAP:              return "heap";
        case D3D12_DRED_ALLOCATION_TYPE_QUERY_HEAP:        return "query heap";
        case D3D12_DRED_ALLOCATION_TYPE_RESOURCE:          return "RESOURCE";
        default:                                           return "object";
    }
}

inline std::string narrow(const wchar_t *w) {
    if (!w) return "<unnamed>";
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return "<unnamed>";
    std::string s(size_t(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Before the device. Both enablements or neither -- the page fault half is the
// one that names resources, and the breadcrumb half is the one that says which
// operation, and a report with only one of them usually cannot be acted on.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// GPU-BASED VALIDATION -- the one that checks what a command CONTAINS.
//
// The ordinary debug layer validates the CALL: types, states, null pointers.
// It has nothing to say about a structure whose bytes are wrong by the time
// the GPU reads them, which is what DXGI_ERROR_DEVICE_RESET -- "a badly formed
// command" -- actually means. GBV patches the command stream and checks the
// contents, and it is the only thing that names an invalid acceleration
// structure input.
//
// IT IS BRUTALLY SLOW -- tens of times, and this scene is already heavy -- so
// it is its own flag rather than part of --debug. V2_GBV=1, and expect a test
// that normally takes two minutes to take considerably longer.
// ---------------------------------------------------------------------------
inline void enableGbv(bool wanted) {
    if (!wanted) return;
    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    if (!d3d12) return;
    using PFN_GetDebugInterface = HRESULT(WINAPI *)(REFIID, void **);
    auto get = reinterpret_cast<PFN_GetDebugInterface>(
        GetProcAddress(d3d12, "D3D12GetDebugInterface"));
    if (!get) return;
    ID3D12Debug1 *dbg = nullptr;
    if (FAILED(get(__uuidof(ID3D12Debug1), reinterpret_cast<void **>(&dbg))) || !dbg) return;
    dbg->EnableDebugLayer();
    dbg->SetEnableGPUBasedValidation(TRUE);
    dbg->SetEnableSynchronizedCommandQueueValidation(TRUE);
    dbg->Release();
    std::printf("  gbv      on -- GPU-based validation, expect this to be very slow\n");
    std::fflush(stdout);
}

inline void enableDred(bool wanted) {
    if (!wanted) return;
    HMODULE d3d12 = LoadLibraryA("d3d12.dll");
    if (!d3d12) return;
    using PFN_GetDebugInterface = HRESULT(WINAPI *)(REFIID, void **);
    auto get = reinterpret_cast<PFN_GetDebugInterface>(
        GetProcAddress(d3d12, "D3D12GetDebugInterface"));
    if (!get) return;
    ID3D12DeviceRemovedExtendedDataSettings *s = nullptr;
    if (FAILED(get(__uuidof(ID3D12DeviceRemovedExtendedDataSettings),
                   reinterpret_cast<void **>(&s))) ||
        !s)
        return;
    s->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    s->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    s->Release();
    detail::dredOn() = true;
    std::printf("  dred     on -- breadcrumbs and page faults will be reported\n");
    std::fflush(stdout);
}

// Remembered once the device exists. Falcor hands out the native handle.
inline void noteDredDevice(void *nativeD3D12Device) {
    detail::dredDevice() = static_cast<ID3D12Device *>(nativeD3D12Device);
}

// ---------------------------------------------------------------------------
// ...AND THE REPORT, on the way out of a removed device.
//
// Written to stderr AND to v2-crash.log, so it lands beside the CPU-side
// report and a bug report carries both halves. Silent when DRED was not on or
// when the device never died -- this is called from the catch in main(), which
// sees every failure and not only this one.
// ---------------------------------------------------------------------------
inline void reportDeviceRemoved(const char *logPath) {
    ID3D12Device *dev = detail::dredDevice();
    if (!dev) return;
    const HRESULT reason = dev->GetDeviceRemovedReason();
    if (reason == S_OK) return;   // the device is fine; this was some other failure

    std::string out = "\n=== v2 DEVICE REMOVED ===\n";
    char line[512];
    std::snprintf(line, sizeof(line), "  reason    0x%08lX%s\n", (unsigned long)reason,
                  reason == DXGI_ERROR_DEVICE_HUNG      ? "  HUNG (a shader did not finish)"
                  : reason == DXGI_ERROR_DEVICE_RESET   ? "  RESET"
                  : reason == DXGI_ERROR_DRIVER_INTERNAL_ERROR ? "  DRIVER INTERNAL ERROR"
                  : reason == DXGI_ERROR_INVALID_CALL   ? "  INVALID CALL (we did something illegal)"
                                                        : "  REMOVED");
    out += line;

    if (!detail::dredOn()) {
        out +=
            "  dred      OFF -- rerun with V2_DRED=1 to find out which GPU operation\n"
            "            faulted and which resource it touched.\n";
    } else {
        ID3D12DeviceRemovedExtendedData1 *dred = nullptr;
        if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D12DeviceRemovedExtendedData1),
                                          reinterpret_cast<void **>(&dred))) &&
            dred) {
            // -- THE LAST THING EACH LIST FINISHED -------------------------
            //
            // BreadcrumbCount is what COMPLETED. The operation that killed the
            // device is the NEXT one, so that is the line worth printing and
            // the reason the two are printed together.
            D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 bc{};
            if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&bc))) {
                int lists = 0;
                for (const D3D12_AUTO_BREADCRUMB_NODE1 *n = bc.pHeadAutoBreadcrumbNode;
                     n && lists < 8; n = n->pNext, ++lists) {
                    const UINT done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
                    std::snprintf(line, sizeof(line),
                                  "  list      %s / %s -- %u of %u operations completed\n",
                                  detail::narrow(n->pCommandQueueDebugNameW).c_str(),
                                  detail::narrow(n->pCommandListDebugNameW).c_str(), done,
                                  n->BreadcrumbCount);
                    out += line;
                    if (done < n->BreadcrumbCount && n->pCommandHistory) {
                        std::snprintf(line, sizeof(line), "    FAULTED ON  %s\n",
                                      detail::opName(n->pCommandHistory[done]));
                        out += line;
                        const UINT from = done > 4 ? done - 4 : 0;
                        for (UINT i = from; i < done; ++i) {
                            std::snprintf(line, sizeof(line), "    before it   %s\n",
                                          detail::opName(n->pCommandHistory[i]));
                            out += line;
                        }
                    }
                }
            }

            // -- AND WHAT LIVED AT THE ADDRESS ------------------------------
            //
            // "Recently freed" is the money line: a resource that was released
            // and whose memory the GPU then read is an aliasing or lifetime
            // bug, and this names it.
            D3D12_DRED_PAGE_FAULT_OUTPUT1 pf{};
            if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&pf)) && pf.PageFaultVA) {
                std::snprintf(line, sizeof(line), "  page fault at 0x%llX\n",
                              (unsigned long long)pf.PageFaultVA);
                out += line;
                int k = 0;
                for (const D3D12_DRED_ALLOCATION_NODE1 *a = pf.pHeadExistingAllocationNode;
                     a && k < 8; a = a->pNext, ++k) {
                    std::snprintf(line, sizeof(line), "    live       %-20s %s\n",
                                  detail::allocName(a->AllocationType),
                                  detail::narrow(a->ObjectNameW).c_str());
                    out += line;
                }
                k = 0;
                for (const D3D12_DRED_ALLOCATION_NODE1 *a = pf.pHeadRecentFreedAllocationNode;
                     a && k < 8; a = a->pNext, ++k) {
                    std::snprintf(line, sizeof(line), "    FREED      %-20s %s\n",
                                  detail::allocName(a->AllocationType),
                                  detail::narrow(a->ObjectNameW).c_str());
                    out += line;
                }
            }
            dred->Release();
        }
    }

    std::fputs(out.c_str(), stderr);
    std::fflush(stderr);
    if (logPath) {
        if (FILE *f = std::fopen(logPath, "ab")) {
            std::fwrite(out.data(), 1, out.size(), f);
            std::fclose(f);
        }
    }
}

// ---------------------------------------------------------------------------
// ...AND MAKE SURE THE REPORT ACTUALLY RUNS.
//
// The first cut called reportDeviceRemoved from the catch in main() and never
// fired once, because FALCOR DOES NOT ALWAYS THROW: gfxReportError prints its
// own stack and ends the process, so the stack stopped at `main`'s call to
// run() and the catch below it was never entered. A reporter that only works
// when the failure is polite is no reporter at all.
//
// So it is hung on every door out:
//   * set_terminate   -- an uncaught or non-std exception, which is what
//                        Falcor's fatal path actually produces here.
//   * atexit          -- an ordinary exit() and the normal end of main.
//   * the catch       -- still there, for the failures that DO throw.
// Each one is idempotent: reportDeviceRemoved asks GetDeviceRemovedReason
// first and returns silently when the device is alive, so a clean shutdown
// prints nothing.
// ---------------------------------------------------------------------------
inline std::string &dredLogPath() {
    static std::string p;
    return p;
}

inline void installDeviceRemovedReporter(const std::string &logPath) {
    dredLogPath() = logPath;
    auto dump = [] { reportDeviceRemoved(dredLogPath().c_str()); };
    // THE ONE THAT ACTUALLY FIRES. Falcor's fatal path ends in
    // `std::quick_exit(1)` -- Core/Error.cpp:123 and :154 -- and quick_exit
    // runs ONLY handlers registered with at_quick_exit. It does not unwind, it
    // does not call terminate, and it does not run atexit handlers, which is
    // why the first two attempts at this reporter printed nothing at all while
    // the device was demonstrably gone.
    std::at_quick_exit(dump);
    // ...and the ordinary doors, for the failures that do not take that one.
    std::atexit(dump);
    static std::terminate_handler prev = nullptr;
    prev = std::set_terminate([] {
        reportDeviceRemoved(dredLogPath().c_str());
        if (prev) prev();
        std::abort();
    });
}

#else
inline void enableDred(bool) {}
inline void installDeviceRemovedReporter(const std::string &) {}
inline void noteDredDevice(void *) {}
inline void reportDeviceRemoved(const char *) {}
#endif

}  // namespace v2
