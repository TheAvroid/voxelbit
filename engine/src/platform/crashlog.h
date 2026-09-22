#pragma once
// ---------------------------------------------------------------------------
// crashlog.h -- WHAT THE ENGINE WAS DOING WHEN IT DIED.
//
// (user 2026-09-21: "when killing a worm, my game crashed. investigate and
//  fix.")
//
// ---------------------------------------------------------------------------
// WHY THIS EXISTS, AND WHY IT IS NOT A LUXURY
// ---------------------------------------------------------------------------
//
// Until now a crash left exactly one artefact: a Windows Error Reporting
// record naming the faulting MODULE and a byte offset into it. For the worm
// report that record said
//
//     Faulting module name: nvngx_dlssg.dll, version: 310.7.0.0
//     Exception code: 0xc0000005   Fault offset: 0x1abd0
//
// -- DLSS Frame Generation, an access violation -- and nothing else. Not the
// call that reached it, not which thread, not what the game was doing. Forty
// of those records have accumulated since 2026-09-19 and every one of them
// says the same eleven words.
//
// THE MINIDUMP WER TAKES IS NOT KEPT. Its .mdmp files live in
// C:\ProgramData\Microsoft\Windows\WER\Temp and are truncated to zero bytes as
// soon as the report is filed; LocalDumps -- the registry key that would keep
// them -- is not configured and needs an administrator to configure. An engine
// that writes its OWN dump needs neither.
//
// SO THIS IS THE THING THAT TURNS "IT CRASHED" INTO A STACK. It costs nothing
// while the game runs: one filter installed at startup, one breadcrumb struct
// written with plain stores, and not a single call until something faults.
//
// ---------------------------------------------------------------------------
// WHAT IT WRITES
// ---------------------------------------------------------------------------
//
//   v2-crash.log   beside the exe, APPENDED to -- one block per fault, with
//                  the exception code, the faulting module and offset, the
//                  breadcrumbs, and a symbolised stack of the faulting thread.
//   v2-crash-<pid>-<n>.dmp   a minidump, if dbghelp will write one.
//
// BOTH GO TO stderr AS WELL, because the console the launcher leaves in the
// taskbar is where the user actually looks -- see v2.bat.
//
// ---------------------------------------------------------------------------
// TWO HANDLERS, AND THE REASON FOR THE SECOND ONE
// ---------------------------------------------------------------------------
//
// SetUnhandledExceptionFilter is the obvious one and it is not enough on its
// own: it is a single process-wide slot, anything loaded later may overwrite
// it, and a driver DLL that wraps the faulting call in its own __try swallows
// the exception before it is ever "unhandled". The NVIDIA present thread is
// exactly that kind of caller.
//
// A VECTORED HANDLER SEES IT FIRST, and unconditionally -- vectored handlers
// run ahead of every frame-based one, before any __try in the chain. This one
// registers LAST (first = 0) so the driver's own handlers still run in front of
// it, logs only the codes that are always fatal, and returns
// EXCEPTION_CONTINUE_SEARCH so it changes nothing about how the fault is then
// handled. It is a tap on the wire, not a handler.
//
// A first-chance fault that somebody then handles is not a crash, so those are
// labelled as such and capped -- see kMaxFirstChance -- and a run that logs
// four of them and carries on is telling you something real about a library
// that uses faults as control flow.
// ---------------------------------------------------------------------------
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#endif

#include "Core/Platform/OS.h"

namespace v2 {

// ---------------------------------------------------------------------------
// THE BREADCRUMBS.
//
// Plain scalars, written where the engine already knows the answer and read
// only from inside the fault handler. No locks and no formatting on the hot
// path: a crumb is a store, which is what makes it affordable once a frame.
//
// `action` is the one string, and it is a fixed buffer rather than a
// std::string on purpose -- the handler runs in a process that has just taken
// an access violation, and a heap it may no longer trust is the last thing it
// should be walking.
// ---------------------------------------------------------------------------
struct CrashCrumbs {
    long frame = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    double simMs = 0.0;
    char action[192] = {0};
};

inline CrashCrumbs &crumbs() {
    static CrashCrumbs c;
    return c;
}

// Once a frame. Three floats and a long -- see the note on the struct.
inline void crumbFrame(long frame, float x, float y, float z, double simMs) {
    CrashCrumbs &c = crumbs();
    c.frame = frame;
    c.x = x;
    c.y = y;
    c.z = z;
    c.simMs = simMs;
}

// ...and at the events worth naming: a kill, a shatter, a death, a level swap.
// Formatted here because these are rare; the per-frame crumb is not.
inline void crumbAction(const char *fmt, ...) {
    CrashCrumbs &c = crumbs();
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(c.action, sizeof(c.action), fmt, ap);
    va_end(ap);
}

#ifdef _WIN32
namespace detail {

inline constexpr int kMaxFirstChance = 4;

inline int &faultCount() {
    static int n = 0;
    return n;
}

inline std::string exeDir() {
    char buf[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p(buf);
    const size_t cut = p.find_last_of("\\/");
    return (cut == std::string::npos) ? std::string(".") : p.substr(0, cut);
}

// WHICH MODULE OWNS AN ADDRESS, and how far into it -- the two halves of what
// WER prints, worked out the same way it does. A driver DLL has no symbols we
// can resolve, so the offset is the only thing that identifies the call site
// across runs: the same offset in two reports is the same bug.
inline std::string moduleAt(void *addr, uintptr_t *offsetOut) {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(addr), &mod) ||
        !mod) {
        if (offsetOut) *offsetOut = reinterpret_cast<uintptr_t>(addr);
        return "<no module>";
    }
    char buf[MAX_PATH] = {0};
    GetModuleFileNameA(mod, buf, MAX_PATH);
    std::string p(buf);
    const size_t cut = p.find_last_of("\\/");
    if (cut != std::string::npos) p = p.substr(cut + 1);
    if (offsetOut)
        *offsetOut = reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(mod);
    return p;
}

inline const char *codeName(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:    return "ACCESS_VIOLATION";
        case EXCEPTION_STACK_OVERFLOW:      return "STACK_OVERFLOW";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:  return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_PRIV_INSTRUCTION:    return "PRIV_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:       return "IN_PAGE_ERROR";
        case 0xC0000409:                    return "FAST_FAIL (stack buffer overrun)";
        case 0xC0000374:                    return "HEAP_CORRUPTION";
        case 0xE06D7363:                    return "C++ exception";
        default:                            return "";
    }
}

// Only the codes that are always a crash. Everything else -- above all
// 0xE06D7363, which is how every C++ throw in Falcor arrives -- is ordinary
// control flow and must not be logged.
inline bool fatalCode(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case 0xC0000409:
        case 0xC0000374:
            return true;
        default:
            return false;
    }
}

// THE DUMP, THROUGH A DLL WE LOAD OURSELVES. dbghelp is already in the process
// -- Falcor's own stack walker pulls it in -- but resolving the entry point by
// hand means this header adds nothing to the link line, and a machine without
// it gets the log and no .dmp rather than a build failure.
using PFN_MiniDumpWriteDump = BOOL(WINAPI *)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                             PMINIDUMP_EXCEPTION_INFORMATION,
                                             PMINIDUMP_USER_STREAM_INFORMATION,
                                             PMINIDUMP_CALLBACK_INFORMATION);

inline std::string writeDump(EXCEPTION_POINTERS *ep, int n) {
    HMODULE dbg = LoadLibraryA("dbghelp.dll");
    if (!dbg) return {};
    auto fn = reinterpret_cast<PFN_MiniDumpWriteDump>(GetProcAddress(dbg, "MiniDumpWriteDump"));
    if (!fn) return {};
    char path[MAX_PATH];
    std::snprintf(path, sizeof(path), "%s\\v2-crash-%lu-%d.dmp", exeDir().c_str(),
                  (unsigned long)GetCurrentProcessId(), n);
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    // ENOUGH TO WALK EVERY THREAD, not a full heap image: the whole question a
    // dump answers here is "which call reached the faulting one", and that is
    // the stacks plus the memory they point into. A full dump of this engine is
    // seven gigabytes -- see the memory note on its footprint -- and nobody
    // would ever open it.
    const MINIDUMP_TYPE type = MINIDUMP_TYPE(MiniDumpWithThreadInfo | MiniDumpWithHandleData |
                                             MiniDumpWithIndirectlyReferencedMemory |
                                             MiniDumpScanMemory | MiniDumpWithUnloadedModules);
    const BOOL ok = fn(GetCurrentProcess(), GetCurrentProcessId(), f, type, &mei, nullptr, nullptr);
    CloseHandle(f);
    return ok ? std::string(path) : std::string{};
}

// -----------------------------------------------------------------------
// THE REPORT ITSELF.
//
// `unhandled` separates the two callers: the vectored tap, which sees a fault
// that may yet be handled by somebody, and the last-chance filter, which sees
// one that nothing handled and is therefore the death.
// -----------------------------------------------------------------------
inline void report(EXCEPTION_POINTERS *ep, bool unhandled) {
    if (!ep || !ep->ExceptionRecord) return;
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    const int n = ++faultCount();

    uintptr_t off = 0;
    const std::string mod = moduleAt(er->ExceptionAddress, &off);

    const std::time_t t = std::time(nullptr);
    char when[64] = {0};
    if (const std::tm *lt = std::localtime(&t))
        std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", lt);

    const CrashCrumbs &c = crumbs();

    char head[1024];
    std::snprintf(head, sizeof(head),
                  "\n=== v2 %s at %s ===\n"
                  "  code      0x%08lX  %s\n"
                  "  where     %s + 0x%llX  (address %p)\n"
                  "  frame     %ld   sim %.1f s\n"
                  "  player    (%.1f, %.1f, %.1f)\n"
                  "  last      %s\n",
                  unhandled ? "CRASH" : "fault (first chance)", when,
                  (unsigned long)er->ExceptionCode, codeName(er->ExceptionCode), mod.c_str(),
                  (unsigned long long)off, er->ExceptionAddress, c.frame, c.simMs * 0.001,
                  double(c.x), double(c.y), double(c.z),
                  c.action[0] ? c.action : "(nothing recorded)");

    // AN ACCESS VIOLATION SAYS WHICH ADDRESS AND WHICH DIRECTION, and both are
    // worth having: a read of a small address is a null dereference, a read of
    // a plausible one is a dangling pointer, and a write is neither.
    char av[192] = {0};
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        const ULONG_PTR kind = er->ExceptionInformation[0];
        std::snprintf(av, sizeof(av), "  access    %s 0x%llX\n",
                      kind == 0 ? "read of" : kind == 1 ? "write to" : "execute at",
                      (unsigned long long)er->ExceptionInformation[1]);
    }

    // THE STACK IS FALCOR'S OWN WALKER, and it is the right one to use: it is
    // already resolving symbols for this build (every GFX failure prints one),
    // and a filter runs ON the faulting thread, so the frames below this call
    // are the frames that faulted.
    std::string stack;
    try {
        stack = Falcor::getStackTrace(0, 48);
    } catch (...) {
        stack = "  (stack unavailable)";
    }

    std::string dump;
    if (unhandled) dump = writeDump(ep, n);

    std::string all = head;
    all += av;
    all += "  stack\n";
    all += stack;
    if (all.empty() || all.back() != '\n') all += '\n';
    if (!dump.empty()) all += "  dump      " + dump + "\n";

    std::fputs(all.c_str(), stderr);
    std::fflush(stderr);

    const std::string log = exeDir() + "\\v2-crash.log";
    if (FILE *f = std::fopen(log.c_str(), "ab")) {
        std::fwrite(all.data(), 1, all.size(), f);
        std::fclose(f);
        std::fprintf(stderr, "v2: crash report appended to %s\n", log.c_str());
        std::fflush(stderr);
    }
}

inline LONG WINAPI vectoredTap(EXCEPTION_POINTERS *ep) {
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    if (!fatalCode(ep->ExceptionRecord->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
    // A run that faults over and over is a library using faults as control
    // flow; say so a few times and then stop, rather than filling the disk.
    if (faultCount() >= kMaxFirstChance) return EXCEPTION_CONTINUE_SEARCH;
    report(ep, /*unhandled=*/false);
    return EXCEPTION_CONTINUE_SEARCH;   // change nothing -- this is a tap
}

inline LONG WINAPI lastChance(EXCEPTION_POINTERS *ep) {
    report(ep, /*unhandled=*/true);
    // EXECUTE_HANDLER, not CONTINUE_SEARCH: the report is written, and letting
    // it fall through to WER buys a dialog, a two-second upload and a truncated
    // dump nobody can read. Exiting here is the same death, quietly.
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace detail
#endif  // _WIN32

// ---------------------------------------------------------------------------
// Installed from main() before anything else exists. Both handlers or neither.
// ---------------------------------------------------------------------------
inline void installCrashHandler() {
#ifdef _WIN32
    AddVectoredExceptionHandler(0 /*last*/, &detail::vectoredTap);
    SetUnhandledExceptionFilter(&detail::lastChance);
#endif
}

}  // namespace v2
