// ---------------------------------------------------------------------------
// launcher.cpp -- voxelbit.exe: one file a player can double-click.
//
// (user 2026-09-21: "can you package all of v1 files into a single .exe file
//  that a player can click to launch the game?")
//
// ---------------------------------------------------------------------------
// WHAT THIS IS
// ---------------------------------------------------------------------------
//
// The shipped game is ~1.3 GB of engine, shaders, voxel art and terrain. That
// cannot be a single PE image -- it is not code -- so it is APPENDED to this
// one. `voxelbit.exe` is this launcher followed by a compressed archive and an
// eight-byte trailer that says where the archive starts. Windows loads the PE
// header at the front and ignores everything past it, which is what makes a
// self-extracting exe possible at all.
//
//   [ this launcher ][ chunk data ... ][ index ][ trailer ]
//                                                  ^ read first, from the end
//
// tools/package.py writes that file. This reads it back.
//
// ---------------------------------------------------------------------------
// WHY IT UNPACKS INSTEAD OF RUNNING FROM MEMORY
// ---------------------------------------------------------------------------
//
// The engine is Falcor, and Falcor loads its shaders, its plugins and the DLSS
// runtime FROM DISK beside the exe -- so there has to be a disk beside an exe.
// Unpacking once into %LOCALAPPDATA% is the version of that which costs the
// player one wait instead of one per launch: the stamp below is the build id,
// so a new build unpacks and an unchanged one starts immediately.
//
// EXTRACTION IS ATOMIC IN THE ONLY WAY THAT MATTERS. `.complete` is written
// LAST and holds the stamp. A run interrupted halfway leaves no `.complete`,
// so the next launch simply extracts again over the top rather than starting a
// game with half its shaders.
//
// ---------------------------------------------------------------------------
// COMPRESSION IS THE ONE WINDOWS ALREADY HAS
// ---------------------------------------------------------------------------
//
// Cabinet.dll's XPRESS_HUFF, through the Compression API that has shipped in
// every Windows since 8. No third-party packer has to be installed to build
// this and none has to be present to run it -- which was the deciding factor,
// because the alternative was asking the user to install 7-Zip to ship their
// own game. Chunked at 8 MB so no single Compress call has to hold a 136 MB
// terrain file, and a chunk that does not shrink is stored verbatim (the high
// bit of its size says so).
// ---------------------------------------------------------------------------
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <compressapi.h>
#include <shlobj.h>

#include <stdio.h>
#include <string>
#include <vector>

#pragma comment(lib, "Cabinet.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdi32.lib")

namespace {

// Must match tools/package.py byte for byte.
const char kMagic[8] = {'V', 'B', 'P', 'K', '0', '0', '0', '1'};
const unsigned kStoredBit = 0x80000000u;

struct Chunk {
    unsigned comp;   // bytes on disk; kStoredBit set means "not compressed"
    unsigned raw;
};

struct Entry {
    std::wstring path;      // relative, with backslashes already
    unsigned long long offset;
    unsigned long long raw;
    std::vector<Chunk> chunks;
};

// -- the little window -----------------------------------------------------
//
// A PROGRESS BAR AND ONE LINE OF TEXT. It exists because unpacking a gigabyte
// takes long enough that a player who saw nothing would double-click again --
// and two launchers extracting into the same directory is the one failure this
// design cannot survive gracefully.
HWND g_wnd = nullptr, g_bar = nullptr, g_text = nullptr;

// -- ...AND THE ONE CASE WITH NO WINDOW AT ALL ----------------------------
//
// `--background` is the engine's own word for "do not appear on screen", and
// it has to mean the same thing here, or the launcher becomes the thing that
// steals the focus the engine is careful not to take. With it set nothing is
// created, nothing is shown, and the game is started minimised and unfocused.
// It is also what a scripted or unattended install wants.
bool g_silent = false;

void say(const wchar_t *s) {
    if (g_text) SetWindowTextW(g_text, s);
}

void progress(int permille) {
    if (g_bar) SendMessageW(g_bar, PBM_SETPOS, permille, 0);
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_CLOSE) return 0;   // no cancelling halfway through
    return DefWindowProcW(h, msg, w, l);
}

void openWindow(HINSTANCE inst) {
    if (g_silent) return;
    INITCOMMONCONTROLSEX ic{sizeof(ic), ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&ic);
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"voxelbitSetup";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);
    const int w = 460, h = 150;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    g_wnd = CreateWindowExW(0, L"voxelbitSetup", L"voxelbit",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, x, y, w, h, nullptr, nullptr,
                            inst, nullptr);
    g_text = CreateWindowExW(0, L"STATIC", L"Unpacking voxelbit...", WS_CHILD | WS_VISIBLE, 20, 22,
                             w - 60, 40, g_wnd, nullptr, inst, nullptr);
    g_bar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE, 20, 70, w - 60, 22,
                            g_wnd, nullptr, inst, nullptr);
    SendMessageW(g_bar, PBM_SETRANGE32, 0, 1000);
    // The default GUI font, because the system one is the 1995 bitmap face.
    HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    SendMessageW(g_text, WM_SETFONT, (WPARAM)f, TRUE);
    ShowWindow(g_wnd, SW_SHOW);
    UpdateWindow(g_wnd);
}

void fail(const wchar_t *what) {
    if (g_wnd) DestroyWindow(g_wnd);
    // A SILENT RUN STILL HAS TO SAY WHY IT FAILED, but to a console rather
    // than to a dialog nobody is there to dismiss.
    if (g_silent) {
        fwprintf(stderr, L"voxelbit: %s\n", what);
        ExitProcess(1);
    }
    MessageBoxW(nullptr, what, L"voxelbit", MB_ICONERROR | MB_OK);
    ExitProcess(1);
}

// -- reading ----------------------------------------------------------------

bool readAt(HANDLE f, unsigned long long off, void *buf, DWORD n) {
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)off;
    if (!SetFilePointerEx(f, li, nullptr, FILE_BEGIN)) return false;
    DWORD got = 0;
    return ReadFile(f, buf, n, &got, nullptr) && got == n;
}

template <class T>
T take(const unsigned char *&p) {
    T v;
    memcpy(&v, p, sizeof(T));
    p += sizeof(T);
    return v;
}

// EVERY PARENT, NOT JUST THE LAST ONE. The archive stores full relative paths
// and nothing guarantees a directory entry comes before the files in it.
void ensureDirs(const std::wstring &full) {
    size_t cut = full.find_last_of(L'\\');
    if (cut == std::wstring::npos) return;
    std::wstring dir = full.substr(0, cut);
    std::vector<std::wstring> stack;
    while (dir.size() > 3 && GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) {
        stack.push_back(dir);
        size_t c = dir.find_last_of(L'\\');
        if (c == std::wstring::npos) break;
        dir = dir.substr(0, c);
    }
    for (size_t i = stack.size(); i-- > 0;) CreateDirectoryW(stack[i].c_str(), nullptr);
}

std::wstring widen(const std::string &s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdLine, int) {
    g_silent = cmdLine && wcsstr(cmdLine, L"--background") != nullptr;

    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);

    HANDLE f = CreateFileW(self, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) fail(L"Could not open voxelbit.exe to read its own payload.");

    LARGE_INTEGER size;
    GetFileSizeEx(f, &size);

    // -- the trailer, from the end ------------------------------------------
    // magic[8] indexOffset[8] indexSize[8] stamp[16] = 40 bytes
    unsigned char tr[40];
    if ((unsigned long long)size.QuadPart < sizeof(tr) ||
        !readAt(f, (unsigned long long)size.QuadPart - sizeof(tr), tr, sizeof(tr)) ||
        memcmp(tr, kMagic, 8) != 0)
        fail(L"This voxelbit.exe has no game data attached.\n\nIt was probably truncated by a "
             L"download or an antivirus scan. Download it again.");

    const unsigned char *tp = tr + 8;
    const unsigned long long indexOff = take<unsigned long long>(tp);
    const unsigned long long indexSize = take<unsigned long long>(tp);
    char stampBuf[17] = {0};
    memcpy(stampBuf, tp, 16);
    const std::wstring stamp = widen(stampBuf);

    // -- where it goes ------------------------------------------------------
    wchar_t *local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
        fail(L"Could not find your Local AppData folder.");
    std::wstring root = std::wstring(local) + L"\\voxelbit\\" + stamp;
    CoTaskMemFree(local);

    const std::wstring gameExe = root + L"\\app\\v1.exe";
    const std::wstring doneFile = root + L"\\.complete";

    // ALREADY UNPACKED? The stamp is in the directory name, so this is only
    // ever asking about THIS build.
    bool haveIt = GetFileAttributesW(doneFile.c_str()) != INVALID_FILE_ATTRIBUTES &&
                  GetFileAttributesW(gameExe.c_str()) != INVALID_FILE_ATTRIBUTES;

    if (!haveIt) {
        openWindow(inst);

        std::vector<unsigned char> index((size_t)indexSize);
        if (!readAt(f, indexOff, index.data(), (DWORD)indexSize))
            fail(L"The game data inside voxelbit.exe is damaged (index unreadable).");

        const unsigned char *ip = index.data();
        const unsigned count = take<unsigned>(ip);
        std::vector<Entry> entries((size_t)count);
        unsigned long long totalRaw = 0;
        for (unsigned i = 0; i < count; ++i) {
            Entry &e = entries[i];
            const unsigned short plen = take<unsigned short>(ip);
            std::string p((const char *)ip, plen);
            ip += plen;
            e.path = widen(p);
            for (auto &c : e.path)
                if (c == L'/') c = L'\\';
            e.offset = take<unsigned long long>(ip);
            e.raw = take<unsigned long long>(ip);
            const unsigned nch = take<unsigned>(ip);
            e.chunks.resize(nch);
            for (unsigned c = 0; c < nch; ++c) {
                e.chunks[c].comp = take<unsigned>(ip);
                e.chunks[c].raw = take<unsigned>(ip);
            }
            totalRaw += e.raw;
        }

        DECOMPRESSOR_HANDLE dec = nullptr;
        if (!CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &dec))
            fail(L"Windows could not start its decompressor (Cabinet.dll).");

        CreateDirectoryW((std::wstring(root).substr(0, root.find_last_of(L'\\'))).c_str(), nullptr);
        ensureDirs(root + L"\\x");

        std::vector<unsigned char> inBuf, outBuf;
        unsigned long long done = 0;
        int lastPermille = -1;

        for (const Entry &e : entries) {
            const std::wstring full = root + L"\\" + e.path;
            ensureDirs(full);
            HANDLE o = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
            if (o == INVALID_HANDLE_VALUE)
                fail((L"Could not write " + full +
                      L"\n\nIs the folder open in another program, or is the disk full?")
                         .c_str());

            unsigned long long at = e.offset;
            for (const Chunk &c : e.chunks) {
                const bool stored = (c.comp & kStoredBit) != 0;
                const unsigned comp = c.comp & ~kStoredBit;
                inBuf.resize(comp);
                if (!readAt(f, at, inBuf.data(), comp))
                    fail(L"The game data inside voxelbit.exe is damaged (short read).");
                at += comp;

                const unsigned char *src = inBuf.data();
                SIZE_T n = comp;
                if (!stored) {
                    outBuf.resize(c.raw);
                    SIZE_T got = 0;
                    if (!Decompress(dec, inBuf.data(), comp, outBuf.data(), c.raw, &got) ||
                        got != c.raw)
                        fail(L"The game data inside voxelbit.exe is damaged (bad chunk).");
                    src = outBuf.data();
                    n = c.raw;
                }
                DWORD wrote = 0;
                if (!WriteFile(o, src, (DWORD)n, &wrote, nullptr) || wrote != n)
                    fail(L"Ran out of disk space while unpacking voxelbit.");
                done += n;

                // A REPAINT PER CHUNK, NOT PER FILE. The archive holds a
                // handful of 100 MB terrain files; per file, the bar would sit
                // still through the longest part of the wait.
                const int permille = totalRaw ? int((done * 1000) / totalRaw) : 1000;
                if (permille != lastPermille) {
                    lastPermille = permille;
                    wchar_t line[128];
                    swprintf(line, 128, L"Unpacking voxelbit...  %llu of %llu MB",
                             done >> 20, totalRaw >> 20);
                    say(line);
                    progress(permille);
                }
            }
            CloseHandle(o);
        }
        CloseDecompressor(dec);

        // LAST, AND ONLY IF EVERYTHING ABOVE SURVIVED.
        HANDLE d = CreateFileW(doneFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_HIDDEN, nullptr);
        if (d != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            WriteFile(d, stampBuf, 16, &w, nullptr);
            CloseHandle(d);
        }
        say(L"Starting voxelbit...");
        progress(1000);
    }
    CloseHandle(f);

    // -- and hand over ------------------------------------------------------
    //
    // VOXELBIT_DATA IS THE CONTRACT WITH THE ENGINE. core/assetroot.h checks it
    // first, so the game does not have to guess where it was unpacked to, and a
    // player who moves voxelbit.exe changes nothing.
    //
    // The working directory is the install root rather than app/, because
    // screenshots and recordings are written relative to it -- see outputPath.
    SetEnvironmentVariableW(L"VOXELBIT_DATA", (root + L"\\data").c_str());

    // -- VOXELBIT_HOME: WHERE THE PLAYER CAN ACTUALLY FIND A FILE ----------
    //
    // The working directory is the unpack root, which is
    // %LOCALAPPDATA%\voxelbit\<16 hex digits> -- so a take recorded in the
    // packaged game was written somewhere no player would ever look, under a
    // name that changes with every build, in a folder the NEXT build is
    // entitled to replace. The video was made correctly and then hidden.
    //
    // This is the directory voxelbit.exe itself sits in, which is the one
    // place the player chose. outputPath writes its recordings there; see
    // app_sun.inl. Unset when a developer runs v1.exe directly, and that is
    // deliberate -- the working directory is the repo then, and takes have
    // always landed in voxelbit/recordings.
    {
        std::wstring home(self);
        const size_t cut = home.find_last_of(L"\\/");
        if (cut != std::wstring::npos) {
            home.resize(cut);
            SetEnvironmentVariableW(L"VOXELBIT_HOME", home.c_str());
        }
    }

    std::wstring cmd = L"\"" + gameExe + L"\"";
    if (cmdLine && *cmdLine) cmd += std::wstring(L" ") + cmdLine;   // v1.exe flags pass straight through

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    if (g_silent) {
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_SHOWMINNOACTIVE;
    }
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');
    // -- NO CONSOLE. NOT A MINIMISED ONE, NONE --------------------------
    //
    // (user 2026-09-21: "hide the terminal in the taskbar, dont open it".)
    //
    // v1.exe IS A CONSOLE SUBSYSTEM BINARY -- Falcor's add_falcor_executable
    // builds it that way, and the engine prints a great deal to stdout that is
    // worth having while developing. Started from here with no flags, Windows
    // therefore gives it a console of its own, and a player who double-clicked
    // a game gets a black terminal window in their taskbar beside it.
    //
    // CREATE_NO_WINDOW, not DETACHED_PROCESS: detached leaves the child with no
    // console at all, and the first printf then fails rather than going nowhere.
    // This gives it a console that is never shown, which is the difference
    // between "quiet" and "broken".
    //
    // THE OUTPUT IS NOT LOST. Anything worth reading after the fact is in
    // v2-crash.log beside the exe -- see crashlog.h -- and a developer runs
    // v1.exe directly rather than through this.
    if (!CreateProcessW(gameExe.c_str(), mutableCmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr,
                        root.c_str(), &si, &pi))
        fail((L"Unpacked, but could not start " + gameExe).c_str());

    // THE WINDOW GOES WHEN THE GAME'S OWN APPEARS, not the instant the process
    // exists -- Falcor spends a few seconds on its device and its shaders, and
    // a bare desktop in between reads as a failed launch.
    if (g_wnd) {
        WaitForInputIdle(pi.hProcess, 20000);
        DestroyWindow(g_wnd);
    }
    // A SILENT RUN IS A SCRIPTED ONE, so it waits and hands back the game's
    // own exit code -- otherwise the caller has nothing to test. An ordinary
    // double-click returns immediately: the player wants their taskbar back.
    DWORD rc = 0;
    if (g_silent) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &rc);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)rc;
}
