#!/bin/bash
# Build tools/laz2las.exe -- the LAZ decompressor.
#
#   bash tools/laszip_build.sh [workdir]
#
# WHY. tools/lasread.py reads LAS with the standard library alone and refuses
# LAZ by name, because LAZ is chunked arithmetic coding and hand-rolling it
# would be wrong in ways that do not announce themselves. This machine has no
# pip, no pacman, no PDAL and no laszip binary, so the options were write a
# decoder or build the reference one. This builds the reference one.
#
# LICENCE: LASzip is **Apache 2.0**. There is no linking constraint -- but it
# is still built as a SEPARATE CLI rather than linked into v2, so the engine
# carries no third-party point-cloud dependency and everything downstream of
# laz2las stays stdlib-only.
#
# FOUR THINGS ARE NEEDED AND NONE OF THEM ARE OBVIOUS:
#
#  1. THE 3.4.3 TAG, NOT master. Master is mid-refactor: mydefs.hpp declares
#     FileDelete taking std::filesystem::path (so it needs -std=c++17 at
#     minimum), and IS_LITTLE_ENDIAN moved into a namespace and stopped being
#     callable, which breaks lasunzipper.cpp and laszipper.cpp.
#  2. -std=c++17. 3.4.3 needs it too.
#  3. SKIP src/lasindex.cpp AND src/lasinterval.cpp. lasindex.cpp #includes
#     "lasreader.hpp", which lives in LAStools and is not in this repository at
#     all, so it cannot be built here; lasinterval.cpp then fails on an
#     ambiguous unordered_map. Both are the .lax spatial index, which a
#     whole-file decompressor never touches.
#  4. ...but laszip_dll.cpp still REFERENCES LASindex, in two ways. Its
#     laszip_read_inside_point calls LASindex::seek_next with LAStools'
#     signature (a compile error), and it constructs a LASindex elsewhere (a
#     link error). So the index read path is disabled and a no-op LASindex is
#     supplied. Every stub method returns FALSE, so if anyone re-enables that
#     path it fails loudly instead of silently returning wrong points.
#
# Do NOT link dll/laszip_api.c: it is the runtime DLL-loading shim and drags in
# windows.h, whose rpcndr.h collides with std::byte under C++17.
set -euo pipefail

WORK="${1:-/tmp/laszip-build}"
OUT="$(cd "$(dirname "$0")" && pwd)/laz2las.exe"
SRC_CPP="$(cd "$(dirname "$0")" && pwd)/laz2las.cpp"
TAG=3.4.3

mkdir -p "$WORK" && cd "$WORK"
if [ ! -d "LASzip-$TAG" ]; then
  echo "fetching LASzip $TAG..."
  curl -sL --fail -o "lz.zip" "https://codeload.github.com/LASzip/LASzip/zip/refs/tags/$TAG"
  python -c "import zipfile;zipfile.ZipFile('lz.zip').extractall('.')"
fi
cd "LASzip-$TAG"

# ---- 4a. disable the spatial-index READ path (compile error) -------------
python - <<'PYEOF'
import io
p = "src/laszip_dll.cpp"
s = io.open(p, encoding="utf-8", errors="replace").read()
old = "      while (laszip_dll->lax_index->seek_next(laszip_dll->reader, laszip_dll->p_count))"
new = "      while (false)   /* PATCHED by laszip_build.sh: no matching LASindex::seek_next here */"
if old in s:
    io.open(p, "w", encoding="utf-8", newline="\n").write(s.replace(old, new, 1))
    print("  patched laszip_read_inside_point")
else:
    print("  laszip_read_inside_point already patched")
PYEOF

# ---- 4b. a no-op LASindex so laszip_dll.o links --------------------------
cat > lasindex_stub.cpp <<'EOF'
// See tools/laszip_build.sh. LASzip 3.4.3 cannot build its own lasindex.cpp
// (it needs LAStools' lasreader.hpp), and the .lax index is not used by a
// whole-file decompressor. Everything here fails rather than pretending.
#include "lasindex.hpp"
LASindex::LASindex() { spatial = 0; interval = 0; have_interval = FALSE;
                       start = end = full = total = cells = 0; }
LASindex::~LASindex() {}
void LASindex::prepare(LASquadtree*, I32) {}
BOOL LASindex::add(const F64, const F64, const U32) { return FALSE; }
void LASindex::complete(U32, I32, const BOOL) {}
BOOL LASindex::read(FILE*) { return FALSE; }
BOOL LASindex::write(FILE*) const { return FALSE; }
BOOL LASindex::read(const char*) { return FALSE; }
BOOL LASindex::append(const char*) const { return FALSE; }
BOOL LASindex::write(const char*) const { return FALSE; }
BOOL LASindex::read(ByteStreamIn*) { return FALSE; }
BOOL LASindex::write(ByteStreamOut*) const { return FALSE; }
BOOL LASindex::intersect_rectangle(const F64, const F64, const F64, const F64) { return FALSE; }
BOOL LASindex::intersect_tile(const F32, const F32, const F32) { return FALSE; }
BOOL LASindex::intersect_circle(const F64, const F64, const F64) { return FALSE; }
BOOL LASindex::get_intervals() { return FALSE; }
BOOL LASindex::has_intervals() { return FALSE; }
EOF

INC="-I src -I include -I include/laszip -I dll"
SRCS=$(ls src/*.cpp | grep -vE "lasindex\.cpp|lasinterval\.cpp")
echo "compiling $(echo "$SRCS" | wc -w) LASzip sources + the stub..."
g++ -O2 -std=c++17 -c $INC $SRCS lasindex_stub.cpp
echo "linking $OUT ..."
g++ -O2 -std=c++17 $INC -o "$OUT" "$SRC_CPP" ./*.o
echo "built $OUT"
"$OUT" 2>&1 | head -2 || true
