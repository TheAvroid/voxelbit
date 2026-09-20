// laz2las -- decompress a LAZ point cloud to plain LAS.
//
//   laz2las <in.laz> <out.las>
//
// WHY THIS EXISTS. tools/lasread.py reads LAS with nothing but the standard
// library, and refuses LAZ by name, because LAZ is chunked arithmetic coding
// and reimplementing it is a serious piece of work that would be wrong in
// subtle ways. This machine has no pip, no pacman, no PDAL and no laszip
// binary, so the choice was write a decoder or build the reference one.
//
// It builds the reference one. LASzip is **Apache 2.0** (not LGPL, which was
// this author's first assumption), so there is no linking constraint at all --
// but it is still kept as a SEPARATE CLI rather than linked into v2, so the
// engine keeps no third-party point-cloud dependency and everything downstream
// of here stays stdlib-only.
//
// BUILDING IT, which took three goes and is worth recording:
//
//   * use the 3.4.3 TAG, not master. Master is mid-refactor: `FileDelete`
//     wants <filesystem> and `IS_LITTLE_ENDIAN()` moved into a namespace and
//     stopped being callable.
//   * skip src/lasindex.cpp and src/lasinterval.cpp. They are the .lax
//     spatial index and they #include "lasreader.hpp", which lives in LAStools
//     and is not in this repository. Decompression does not use them.
//   * ONE PATCH is needed even so: laszip_dll.cpp's laszip_read_inside_point
//     calls LASindex::seek_next with LAStools' signature. Disabled -- see
//     tools/laszip_build.sh, which does all of this reproducibly.
//
// Everything that actually matters compiled untouched, including
// lasreaditemcompressed_v4 -- the LAS 1.4 LAYERED compressor, which is what
// point data record format 6 needs and therefore what NOAA's topobathymetric
// surveys need.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "laszip_api.h"

static int fail(laszip_POINTER h, const char *what) {
    laszip_CHAR *msg = nullptr;
    if (h) laszip_get_error(h, &msg);
    fprintf(stderr, "%s: %s\n", what, msg ? msg : "(no detail)");
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
                "laz2las <in.laz> <out.las>\n\n"
                "Decompresses to plain LAS so tools/lasread.py can read it.\n");
        return 2;
    }
    const char *inPath = argv[1], *outPath = argv[2];

    laszip_POINTER reader = nullptr;
    if (laszip_create(&reader)) return fail(nullptr, "laszip_create");

    laszip_BOOL compressed = 0;
    if (laszip_open_reader(reader, inPath, &compressed))
        return fail(reader, "cannot open input");
    if (!compressed)
        fprintf(stderr, "note: %s is not actually compressed; copying anyway\n", inPath);

    laszip_header *hdr = nullptr;
    if (laszip_get_header_pointer(reader, &hdr)) return fail(reader, "header");
    const long long n = (hdr->number_of_point_records
                             ? (long long)hdr->number_of_point_records
                             : (long long)hdr->extended_number_of_point_records);
    printf("in    %s\n      LAS %d.%d  point format %d  %lld points  %u VLRs\n",
           inPath, hdr->version_major, hdr->version_minor,
           hdr->point_data_format, n, hdr->number_of_variable_length_records);

    laszip_point *pt = nullptr;
    if (laszip_get_point_pointer(reader, &pt)) return fail(reader, "point pointer");

    // ---- the writer, sharing the reader's header -------------------------
    laszip_POINTER writer = nullptr;
    if (laszip_create(&writer)) return fail(nullptr, "laszip_create (writer)");
    if (laszip_set_header(writer, hdr)) return fail(writer, "set_header");
    // FALSE is the whole point: write it back out uncompressed.
    if (laszip_open_writer(writer, outPath, 0)) return fail(writer, "cannot open output");

    laszip_point *wp = nullptr;
    if (laszip_get_point_pointer(writer, &wp)) return fail(writer, "write point pointer");

    long long done = 0;
    for (long long i = 0; i < n; ++i) {
        if (laszip_read_point(reader)) return fail(reader, "read_point");
        // Copy the whole record, extra bytes included -- a tree id or a
        // classification lives in there and silently dropping it would make
        // the output look fine and be useless.
        *wp = *pt;
        if (laszip_write_point(writer)) return fail(writer, "write_point");
        ++done;
        if ((done & 0xFFFFF) == 0) {
            printf("\r      %lld / %lld", done, n);
            fflush(stdout);
        }
    }
    printf("\r      %lld points written        \n", done);

    if (laszip_close_writer(writer)) return fail(writer, "close_writer");
    laszip_destroy(writer);
    if (laszip_close_reader(reader)) return fail(reader, "close_reader");
    laszip_destroy(reader);
    printf("out   %s\n", outPath);
    return 0;
}
