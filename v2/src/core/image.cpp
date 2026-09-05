// The single translation unit that instantiates stb_image_write. Kept apart
// from any header so the 70 KB of implementation is compiled exactly once.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO_UNUSED
#include "../ext/stb_image_write.h"

#include "image.h"

namespace v2 {

bool writePng(const std::string &path, const std::vector<unsigned char> &rgba, int w, int h) {
    // The tonemap kernel writes uchar4 because a four-byte store is one
    // transaction and a three-byte store is not. PNG wants three, so the alpha
    // is dropped here rather than costing bandwidth on every frame the viewer
    // draws and never writes.
    std::vector<unsigned char> rgb(size_t(w) * h * 3);
    for (size_t i = 0, n = size_t(w) * h; i < n; ++i) {
        rgb[i * 3 + 0] = rgba[i * 4 + 0];
        rgb[i * 3 + 1] = rgba[i * 4 + 1];
        rgb[i * 3 + 2] = rgba[i * 4 + 2];
    }
    return stbi_write_png(path.c_str(), w, h, 3, rgb.data(), w * 3) != 0;
}

}  // namespace v2
