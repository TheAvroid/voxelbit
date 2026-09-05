// ---------------------------------------------------------------------------
// image.h -- getting a finished frame onto disk.
//
// Far smaller than v4's file of the same name, and the reason is where the work
// moved. v4 kept a Film of linear radiance on the host, resolved it, ran a tone
// curve over it and tabulated the sRGB transfer function to make that curve
// affordable. All of that is a GPU kernel now (optix/tonemap.cu), so what is
// left here is the two file formats and nothing else.
//
// The PFM path still writes LINEAR radiance, deliberately. It is the only way
// to see what the tracer actually produced -- everything else in the pipeline
// has a tone curve baked into it by the time a human sees it.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace v2 {

// RGBA8 as the tonemap kernel leaves it, written as RGB.
bool writePng(const std::string &path, const std::vector<unsigned char> &rgba, int w, int h);

// A portable float image, for inspecting the raw linear result. Input is the
// float4 buffer the renderer hands back; the alpha channel is dropped.
inline bool writePfm(const std::string &path, const std::vector<float> &rgba, int w, int h) {
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "PF\n%d %d\n-1.0\n", w, h);  // -1.0 marks little-endian
    for (int y = h - 1; y >= 0; --y)             // PFM rows run bottom-up
        for (int x = 0; x < w; ++x) {
            const size_t i = (size_t(y) * w + x) * 4;
            const float px[3] = {rgba[i], rgba[i + 1], rgba[i + 2]};
            std::fwrite(px, sizeof(float), 3, f);
        }
    std::fclose(f);
    return true;
}

}  // namespace v2
