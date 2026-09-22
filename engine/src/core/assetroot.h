#pragma once
// ---------------------------------------------------------------------------
// assetroot.h -- WHERE THE GAME'S FILES ARE, ANSWERED ONCE.
//
// (user 2026-09-21: "can you package all of v2 files into a single .exe file
//  that a player can click to launch the game?")
//
// ---------------------------------------------------------------------------
// WHY THIS HAD TO EXIST BEFORE ANYTHING COULD BE PACKAGED
// ---------------------------------------------------------------------------
//
// Forty-four asset paths in this engine were absolute and began `C:/voxelbit/`.
// That is invisible while the only machine that runs it is the one it was
// written on, and it is the whole ballgame the moment somebody else
// double-clicks it: a player has no C:\voxelbit, so the first .vox load fails
// and the wood comes up empty. No amount of packaging fixes that from the
// outside -- the exe has to be able to say where its own files are.
//
// So every one of those literals now goes through asset(), and this header is
// the only place that knows what an absolute path looks like.
//
// ---------------------------------------------------------------------------
// THE LAYOUT IS THE SAME IN BOTH PLACES, AND THAT IS THE POINT
// ---------------------------------------------------------------------------
//
// A relative path handed to asset() -- "game/assets/decoration/wheat.vox" --
// is the path THE REPOSITORY ALREADY USES. The packaged build copies that
// shape verbatim into its data directory rather than inventing a flat one, so
// a path that works in the dev tree works in the shipped game and neither
// needs a translation table:
//
//   dev        C:/voxelbit/                 game/assets/...   engine/assets/dem/...
//   packaged   %LOCALAPPDATA%/voxelbit/<v>/ game/assets/...   engine/assets/dem/...
//
// ---------------------------------------------------------------------------
// HOW THE ROOT IS FOUND, IN ORDER
// ---------------------------------------------------------------------------
//
//   1. VOXELBIT_DATA, if it is set and has a game/assets under it. This is the
//      override the packaged launcher uses, and it is also how a developer
//      points a build at a different content tree without rebuilding.
//   2. UP FROM THE EXE. build/bin/Release/v1.exe is four levels under the
//      repository root, so a dev build finds C:/voxelbit with no configuration
//      at all and nothing about the daily loop changes.
//   3. BESIDE THE EXE. A packaged layout puts the whole tree in `data/` next to
//      the launcher, which is what a portable unpack looks like.
//   4. C:/voxelbit. The historical answer, kept as the last resort so a build
//      run from somewhere unexpected on THIS machine behaves as it always did.
//
// RESOLVED ONCE, on first use, and reported so a wrong answer is visible in
// the log rather than as forty "cannot open" lines. The probe is `game/assets`
// in every case: a directory that exists but holds no art is the failure mode
// that would otherwise reach the player as an empty world.
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "Core/Platform/OS.h"

namespace v2 {

namespace detail {

inline bool looksLikeRoot(const std::filesystem::path &p) {
    if (p.empty()) return false;
    std::error_code ec;
    return std::filesystem::is_directory(p / "game" / "assets", ec);
}

inline std::string resolveDataRoot() {
    std::error_code ec;

    // 1. The explicit override.
    if (const char *env = std::getenv("VOXELBIT_DATA")) {
        const std::filesystem::path p(env);
        if (looksLikeRoot(p)) return p.generic_string();
    }

    // 2/3. Relative to the executable. getRuntimeDirectory is Falcor's own
    // answer for "where is the exe", which is already used for the shader and
    // DLSS lookups a few files over -- so this cannot disagree with them.
    const std::filesystem::path exeDir = Falcor::getRuntimeDirectory();
    if (!exeDir.empty()) {
        // BESIDE FIRST. A packaged tree has both `data/game/assets` AND, if it
        // were ever unpacked inside a checkout, the checkout above it; the
        // one the launcher laid down is the one it means.
        if (looksLikeRoot(exeDir / "data")) return (exeDir / "data").generic_string();
        std::filesystem::path up = exeDir;
        for (int i = 0; i < 6 && !up.empty(); ++i) {
            if (looksLikeRoot(up)) return up.generic_string();
            const std::filesystem::path parent = up.parent_path();
            if (parent == up) break;
            up = parent;
        }
    }

    // 4. The historical answer.
    return "C:/voxelbit";
}

}  // namespace detail

// The content root, without a trailing slash. Resolved once.
inline const std::string &dataRoot() {
    static const std::string root = [] {
        std::string r = detail::resolveDataRoot();
        std::printf("  data     %s\n", r.c_str());
        std::fflush(stdout);
        return r;
    }();
    return root;
}

// ONE FILE OR DIRECTORY UNDER IT. `rel` is repository-relative and must not
// begin with a slash -- see the layout note above; the two trees are the same
// shape so that this is the whole of the translation.
inline std::string asset(const char *rel) { return dataRoot() + "/" + rel; }
inline std::string asset(const std::string &rel) { return dataRoot() + "/" + rel; }

}  // namespace v2
