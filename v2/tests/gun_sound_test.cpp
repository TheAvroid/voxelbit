// ---------------------------------------------------------------------------
// gun_sound_test.cpp -- does the gun's bank decode, and is the bang at the
// front of it?
//
// WHY THIS EXISTS AND WHY IT IS NOT A SCREENSHOT. The engine refuses to open
// an audio device under --background (app_load.inl), which is the only way a
// verification run is allowed to start -- so "tool sounds N cues" never prints
// in a checkable run and the loading of a cue cannot be observed from a render
// at all. The two ways a sound cue fails silently are both here instead:
//
//   THE FILE DOES NOT DECODE. Media Foundation opens what it opens; a
//   container it will not read comes back as an empty buffer and ToolSounds
//   keeps a -1 handle, which plays nothing and says nothing.
//
//   THE FILE DECODES AND THE SOUND IS NOT AT THE FRONT OF IT. This is the one
//   that bit: the handed-over bullet.mp4 is a 2.95 s screen capture whose bang
//   starts at 1.18 s, and audio.h trims at most 100 ms of codec padding on
//   purpose. Wired whole, every shot would have arrived a second and a fifth
//   after the trigger -- a cue that loads, plays, and is wrong.
//
// So this asserts the SHAPE of each cue: that it decodes, that it is about as
// long as it should be, and that its energy starts within the first few
// frames. Build and run:
//
//     g++ -std=c++17 -O1 -I src tests/gun_sound_test.cpp -o build/gun_sound_test.exe \
//         -lmfplat -lmfreadwrite -lmfuuid -lole32
//     build/gun_sound_test.exe
// ---------------------------------------------------------------------------
#include "platform/audio.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

struct Shape {
    double ms = 0.0;
    double onsetMs = -1.0;  // first frame above -45 dBFS
    double peak = 0.0;
};

Shape measure(const std::vector<int16_t> &pcm) {
    Shape s;
    const size_t frames = pcm.size() / 2;
    s.ms = 1000.0 * double(frames) / 48000.0;
    const double gate = 0.0056;  // -45 dBFS, the threshold the cut was made at
    for (size_t i = 0; i < frames; ++i) {
        const double a = std::max(std::abs(double(pcm[i * 2])), std::abs(double(pcm[i * 2 + 1]))) /
                         32768.0;
        if (a > s.peak) s.peak = a;
        if (s.onsetMs < 0.0 && a > gate) s.onsetMs = 1000.0 * double(i) / 48000.0;
    }
    return s;
}

int fails = 0;

void check(const char *what, bool ok) {
    std::printf("  %-46s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) ++fails;
}

// One cue: it must decode, be within `loMs`..`hiMs` long, and start sounding
// within `onsetMs` of its own beginning.
void cue(const std::string &path, const char *name, double loMs, double hiMs, double onsetMs) {
    std::vector<int16_t> pcm;
    const bool got = vb::decodeToPcm48Stereo(path, &pcm) && !pcm.empty();
    std::printf("%s  (%s)\n", name, path.c_str());
    check("decodes to PCM", got);
    if (!got) return;
    const Shape s = measure(pcm);
    std::printf("    %.0f ms, peak %.3f, onset %.0f ms\n", s.ms, s.peak, s.onsetMs);
    char msg[128];
    std::snprintf(msg, sizeof(msg), "length is %.0f..%.0f ms", loMs, hiMs);
    check(msg, s.ms >= loMs && s.ms <= hiMs);
    std::snprintf(msg, sizeof(msg), "sounds within %.0f ms of its start", onsetMs);
    check(msg, s.onsetMs >= 0.0 && s.onsetMs <= onsetMs);
    check("peak is above -30 dBFS", s.peak > 0.0316);
}

}  // namespace

int main(int argc, char **argv) {
    const std::string dir = argc > 1 ? argv[1] : "C:/voxelbit/game/sound";
    std::printf("gun sound test -- %s\n\n", dir.c_str());
    // THE SHOT. 410 ms is the cut; the window is wide enough that a re-cut on
    // the same bang passes and a whole 2.95 s capture does not. 50 ms of onset
    // is three engine frames -- past that the bang is late enough to hear.
    cue(dir + "/gun/bullet.mp4", "the shot", 300.0, 700.0, 50.0);
    // THE ROUND GOING IN -- the sandbox's own pickup, shared. Its length is
    // not this change's to police, only that it is there and prompt.
    cue(dir + "/pick_up.mp4", "a round going in", 400.0, 1500.0, 50.0);
    std::printf("\n%s\n", fails ? "FAILED" : "all ok");
    return fails ? 1 : 0;
}
