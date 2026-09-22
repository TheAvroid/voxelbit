// ---------------------------------------------------------------------------
// audiotap.h -- what the game is playing, as PCM, so the recorder can keep it.
//
// ---------------------------------------------------------------------------
// WHY AN XAPO AND NOT LOOPBACK CAPTURE.
//
// The obvious way to record a game's sound on Windows is WASAPI loopback: open
// the render endpoint with AUDCLNT_STREAMFLAGS_LOOPBACK and read back whatever
// the speakers are being sent. It is less code than this and it works.
//
// It also records EVERYTHING ELSE. A loopback capture of the endpoint picks up
// the browser, the music the player has on, a notification chime -- all of it,
// baked into the take with no way to take it out afterwards. For a recorder
// whose whole job is "show someone this game" that is the wrong default, and it
// is not a setting you can offer, because by the time you hear the problem the
// take is already made.
//
// So the tap goes on OUR mastering voice instead. An XAPO in the mastering
// voice's effect chain sees exactly the mix XAudio2 is about to hand the
// device -- the ambience, the tools, the pickup, at their real levels, after
// every gain -- and sees nothing that did not come from this engine.
//
// ---------------------------------------------------------------------------
// IT RUNS ON THE AUDIO THREAD, WHICH DECIDES EVERYTHING ABOUT IT.
//
// Process() is called from XAudio2's own thread on a deadline. Miss it and the
// player hears a click, which is worse than a missing recording. So the tap
// does exactly one thing -- copy floats into a ring -- and it does it without
// locking, allocating, or calling anything that might.
//
// The ring is single-producer single-consumer: the audio thread writes, the
// recorder's thread reads, and the two indices are atomics. That is the whole
// synchronisation. If the reader falls behind the writer OVERWRITES, and drops
// are counted rather than hidden -- a recorder that silently invents silence is
// worse than one that can tell you it lost 40 ms.
//
// IXAPO IS IMPLEMENTED BY HAND rather than derived from CXAPOBase, which does
// most of it: xapobase.lib would be a new link-time dependency for about eighty
// lines of boilerplate, and this file is the only thing that would ever use it.
// ---------------------------------------------------------------------------
#pragma once

#include <windows.h>
#include <xapo.h>
#include <xaudio2.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

// `vb`, not `v2`: this sits with audio.h, mfvideo.h and dynamics.h, which are
// the Windows-media corner of the engine and share their own namespace.
namespace vb {

// ---------------------------------------------------------------------------
// The ring the audio thread writes into.
//
// Sized in FRAMES rather than seconds so the arithmetic downstream is exact.
// Four seconds at 48 kHz stereo is 1.5 MB, which is nothing, and it means the
// recorder can miss a great many frames of its own before this matters -- a
// long stall while a chunk builds must not cost the take its sound.
// ---------------------------------------------------------------------------
class AudioRing {
  public:
    void reset(int channels, int rate) {
        ch_ = channels > 0 ? channels : 2;
        rate_ = rate > 0 ? rate : 48000;
        buf_.assign(size_t(rate_) * size_t(ch_) * 4u, 0.0f);
        w_.store(0, std::memory_order_relaxed);
        r_.store(0, std::memory_order_relaxed);
        lost_.store(0, std::memory_order_relaxed);
    }

    int channels() const { return ch_; }
    int rate() const { return rate_; }
    uint64_t lost() const { return lost_.load(std::memory_order_relaxed); }

    // AUDIO THREAD. No lock, no allocation, no branch that can block.
    void write(const float *src, int frames) {
        if (buf_.empty() || frames <= 0) return;
        const size_t n = size_t(frames) * size_t(ch_);
        const size_t cap = buf_.size();
        const uint64_t w = w_.load(std::memory_order_relaxed);
        const uint64_t r = r_.load(std::memory_order_acquire);
        if (w - r + n > cap) {
            // The reader is behind. Say so rather than stalling the device.
            lost_.fetch_add(n / size_t(ch_), std::memory_order_relaxed);
        }
        size_t at = size_t(w % cap);
        const size_t first = cap - at < n ? cap - at : n;
        std::memcpy(&buf_[at], src, first * sizeof(float));
        if (first < n) std::memcpy(&buf_[0], src + first, (n - first) * sizeof(float));
        w_.store(w + n, std::memory_order_release);
    }

    // A GAP IS WRITTEN, NOT SKIPPED. XAudio2 hands a silent buffer with its
    // contents undefined, so there is nothing to copy -- but the frames still
    // happened, and leaving them out would shorten the audio against the video
    // by exactly the length of every quiet passage in the take.
    void writeSilence(int frames) {
        if (buf_.empty() || frames <= 0) return;
        const size_t n = size_t(frames) * size_t(ch_);
        const size_t cap = buf_.size();
        const uint64_t w = w_.load(std::memory_order_relaxed);
        const uint64_t r = r_.load(std::memory_order_acquire);
        if (w - r + n > cap) lost_.fetch_add(n / size_t(ch_), std::memory_order_relaxed);
        size_t at = size_t(w % cap);
        const size_t first = cap - at < n ? cap - at : n;
        std::memset(&buf_[at], 0, first * sizeof(float));
        if (first < n) std::memset(&buf_[0], 0, (n - first) * sizeof(float));
        w_.store(w + n, std::memory_order_release);
    }

    // RECORDER THREAD. Returns frames actually taken.
    int read(float *dst, int maxFrames) {
        if (buf_.empty() || maxFrames <= 0) return 0;
        const size_t cap = buf_.size();
        const uint64_t w = w_.load(std::memory_order_acquire);
        uint64_t r = r_.load(std::memory_order_relaxed);
        // A writer that has lapped the reader leaves it pointing at a hole; skip
        // to the oldest sample still intact rather than reading torn audio.
        if (w - r > cap) r = w - cap;
        size_t have = size_t(w - r);
        const size_t want = size_t(maxFrames) * size_t(ch_);
        if (have > want) have = want;
        if (have == 0) return 0;
        size_t at = size_t(r % cap);
        const size_t first = cap - at < have ? cap - at : have;
        std::memcpy(dst, &buf_[at], first * sizeof(float));
        if (first < have) std::memcpy(dst + first, &buf_[0], (have - first) * sizeof(float));
        r_.store(r + have, std::memory_order_release);
        return int(have / size_t(ch_));
    }

  private:
    std::vector<float> buf_;
    std::atomic<uint64_t> w_{0}, r_{0};
    std::atomic<uint64_t> lost_{0};
    int ch_ = 2, rate_ = 48000;
};

// ---------------------------------------------------------------------------
// The effect itself. Passes audio through untouched and copies it on the way.
// ---------------------------------------------------------------------------
class AudioTap final : public IXAPO {
  public:
    explicit AudioTap(AudioRing *ring) : ring_(ring) {}

    void arm(bool on) { armed_.store(on, std::memory_order_relaxed); }

    // ---- IUnknown ---------------------------------------------------------
    //
    // The lifetime is OURS, not COM's: AudioDevice holds this for as long as
    // the mastering voice exists and destroys it after. AddRef/Release still
    // have to be honest, because XAudio2 holds a reference while the chain is
    // set, but the object is not deleted on the last release -- releasing an
    // effect XAudio2 still has in a chain would be the crash this avoids.
    HRESULT __stdcall QueryInterface(REFIID iid, void **out) override {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IXAPO)) {
            *out = static_cast<IXAPO *>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG __stdcall AddRef() override { return ++refs_; }
    ULONG __stdcall Release() override {
        const ULONG n = --refs_;
        return n;  // deliberately not delete -- see the note above
    }

    // ---- IXAPO ------------------------------------------------------------
    HRESULT __stdcall GetRegistrationProperties(XAPO_REGISTRATION_PROPERTIES **out) override {
        if (!out) return E_POINTER;
        auto *p = static_cast<XAPO_REGISTRATION_PROPERTIES *>(
            ::CoTaskMemAlloc(sizeof(XAPO_REGISTRATION_PROPERTIES)));
        if (!p) return E_OUTOFMEMORY;
        std::memset(p, 0, sizeof(*p));
        p->clsid = __uuidof(IXAPO);
        wcscpy_s(p->FriendlyName, L"v2 tap");
        wcscpy_s(p->CopyrightInfo, L"");
        p->MajorVersion = 1;
        p->MinorVersion = 0;
        // IN PLACE and never a format converter: it is a wire with a listener
        // on it, so the flags say the input and output are the same buffer and
        // the same format, and XAudio2 gives it exactly that.
        p->Flags = XAPO_FLAG_INPLACE_REQUIRED | XAPO_FLAG_CHANNELS_MUST_MATCH |
                   XAPO_FLAG_FRAMERATE_MUST_MATCH | XAPO_FLAG_BITSPERSAMPLE_MUST_MATCH |
                   XAPO_FLAG_BUFFERCOUNT_MUST_MATCH | XAPO_FLAG_INPLACE_SUPPORTED;
        p->MinInputBufferCount = 1;
        p->MaxInputBufferCount = 1;
        p->MinOutputBufferCount = 1;
        p->MaxOutputBufferCount = 1;
        *out = p;
        return S_OK;
    }
    // ANY FORMAT THE VOICE IS ALREADY USING. A tap does not care what the mix
    // looks like -- it copies it -- and the registration flags above already
    // pin the input to the output, so XAudio2 only ever offers the mastering
    // voice's own format. Accepting it is the whole answer; nulling the
    // suggestion says "no counter-proposal", which is what S_OK means here.
    HRESULT __stdcall IsInputFormatSupported(const WAVEFORMATEX *, const WAVEFORMATEX *,
                                             WAVEFORMATEX **sup) override {
        if (sup) *sup = nullptr;
        return S_OK;
    }
    HRESULT __stdcall IsOutputFormatSupported(const WAVEFORMATEX *, const WAVEFORMATEX *,
                                              WAVEFORMATEX **sup) override {
        if (sup) *sup = nullptr;
        return S_OK;
    }
    HRESULT __stdcall Initialize(const void *, UINT32) override { return S_OK; }
    void __stdcall Reset() override {}
    HRESULT __stdcall LockForProcess(UINT32, const XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS *in,
                                     UINT32,
                                     const XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS *) override {
        if (in && in->pFormat) {
            ch_ = in->pFormat->nChannels;
            rate_ = int(in->pFormat->nSamplesPerSec);
        }
        return S_OK;
    }
    void __stdcall UnlockForProcess() override {}

    // ONE IN, ONE OUT. A tap neither buffers nor resamples, so the frame count
    // is the frame count -- these exist for effects with latency or a rate
    // change, and saying so plainly is what tells XAudio2 this has neither.
    UINT32 __stdcall CalcInputFrames(UINT32 outputFrameCount) override {
        return outputFrameCount;
    }
    UINT32 __stdcall CalcOutputFrames(UINT32 inputFrameCount) override {
        return inputFrameCount;
    }

    // THE ONE THING IT DOES. Passes through by doing nothing to the buffer --
    // the chain is in place, so the output IS the input -- and copies it.
    void __stdcall Process(UINT32, const XAPO_PROCESS_BUFFER_PARAMETERS *in, UINT32,
                           XAPO_PROCESS_BUFFER_PARAMETERS *out, BOOL) override {
        if (out && in) out->ValidFrameCount = in->ValidFrameCount;
        if (out && in) out->BufferFlags = in->BufferFlags;
        if (!armed_.load(std::memory_order_relaxed) || !ring_ || !in) return;
        // SILENT IS STILL AUDIO. XAPO_BUFFER_SILENT means the buffer's contents
        // are undefined rather than zero, so copying it would record whatever
        // was last in that memory. A gap in the take has to be written as
        // actual silence or the recording drifts against the video.
        const UINT32 frames = in->ValidFrameCount;
        if (frames == 0) return;
        if (in->BufferFlags == XAPO_BUFFER_SILENT) {
            ring_->writeSilence(int(frames));
            return;
        }
        ring_->write(static_cast<const float *>(in->pBuffer), int(frames));
    }

    int channels() const { return ch_; }
    int rate() const { return rate_; }

  private:
    AudioRing *ring_ = nullptr;
    std::atomic<bool> armed_{false};
    std::atomic<ULONG> refs_{1};
    int ch_ = 2, rate_ = 48000;
};

}  // namespace vb
