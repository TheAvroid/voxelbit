// ---------------------------------------------------------------------------
// audio.h -- the wood's own sound, and the first audio v2 has ever had.
//
// "There is no audio track: v2 has no audio" is what render/recorder.h says a
// few files over, and it was true until this. What is here is deliberately the
// SMALLEST thing that can be true instead: one looping stereo bed whose volume
// follows the canopy. No mixer, no 3-D panning, no event sounds, no voice
// pool. A second sound is the moment to grow those, and not before.
//
// ---------------------------------------------------------------------------
// WHY MEDIA FOUNDATION AND XAUDIO2, AND NO THIRD-PARTY DEPENDENCY
//
// Media Foundation is already the whole recorder (render/mfvideo.h) and is
// already linked, so its source reader decodes the mp3 for free: the same
// mfInit(), the same widen(), the same ComPtr. Bringing in minimp3 or dr_mp3
// to re-solve a problem the process is already carrying the code for is the
// kind of dependency that gets added because it is small rather than because
// it is needed.
//
// XAudio2 is the other half and costs one import library that ships with
// Windows -- xaudio2.lib in the SDK is the stub for the in-box XAudio2.9. It
// is what an infinitely looping buffer with a settable volume looks like when
// you do not write a mixer, and that is exactly the shape of this feature.
//
// ---------------------------------------------------------------------------
// THE WHOLE FILE IS DECODED INTO RAM AND NEVER STREAMED
//
// Five minutes of 48 kHz stereo 16-bit is 57 MB. That is a real number and it
// is worth being explicit that it was chosen and not overlooked: streaming it
// would need a feeder thread, a buffer queue, an underrun policy and a way to
// wake all of that at the loop seam -- machinery whose failure mode is a
// dropout in the one sound the engine makes. A resident buffer with
// XAUDIO2_LOOP_INFINITE has no seam logic at all: XAudio2 wraps it inside the
// mixer and the sound cannot stop. 57 MB against a renderer that reserves
// gigabytes of VRAM is not the constraint here.
//
// ---------------------------------------------------------------------------
// IT PLAYS FROM LOAD AND NEVER RESTARTS
//
// Start() is called once, at volume zero, and the volume is what moves. The
// obvious alternative -- start the voice on entering a wood, stop it on
// leaving -- restarts the bed from its first sample every time, so walking in
// and out of a clearing replays the same opening bird call over and over and
// the loop announces itself. Letting it run underneath silently costs one
// voice and makes the wood sound continuous, which is the point.
// ---------------------------------------------------------------------------
#pragma once

#include "render/dynamics.h"  // tameLoudPeaks
#include "render/mfvideo.h"  // mfInit, widen, ComPtr -- and mfapi/mfidl/mfreadwrite

#include <xaudio2.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace vb {

// ---------------------------------------------------------------------------
// Decode any file Media Foundation can open into 48 kHz stereo 16-bit PCM.
//
// The reader is asked for PCM at a fixed rate and channel count rather than
// being asked what it has: MF inserts its own resampler and channel matrix to
// satisfy the request, which is one less thing for this file to implement and
// removes the possibility of the mastering voice being handed a format it
// then has to convert anyway.
// ---------------------------------------------------------------------------
inline bool decodeToPcm48Stereo(const std::string &path, std::vector<int16_t> *out) {
    out->clear();
    if (!mfInit()) return false;

    ComPtr<IMFSourceReader> rd;
    HRESULT hr = ::MFCreateSourceReaderFromURL(widen(path).c_str(), nullptr, &rd);
    if (FAILED(hr)) {
        std::fprintf(stderr, "v2: cannot open audio %s (0x%08lx)\n", path.c_str(),
                     (unsigned long)hr);
        return false;
    }

    rd->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    rd->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);

    ComPtr<IMFMediaType> want;
    if (FAILED(::MFCreateMediaType(&want))) return false;
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    want->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    want->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
    want->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    hr = rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr,
                                 want.Get());
    if (FAILED(hr)) {
        std::fprintf(stderr, "v2: %s is not decodable to 48k stereo PCM (0x%08lx)\n",
                     path.c_str(), (unsigned long)hr);
        return false;
    }

    for (;;) {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        hr = rd->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags,
                            nullptr, &sample);
        if (FAILED(hr)) return false;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        // A format change mid-file would silently corrupt the buffer from that
        // point on, so it is a hard stop rather than something to paper over.
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            std::fprintf(stderr, "v2: %s changes format mid-stream -- not loaded\n",
                         path.c_str());
            return false;
        }
        if (!sample) continue;  // a gap; MF hands back no sample and keeps going

        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->ConvertToContiguousBuffer(&buf))) return false;
        BYTE *p = nullptr;
        DWORD len = 0;
        if (FAILED(buf->Lock(&p, nullptr, &len))) return false;
        const size_t n = size_t(len) / sizeof(int16_t);
        const size_t at = out->size();
        out->resize(at + n);
        std::memcpy(out->data() + at, p, n * sizeof(int16_t));
        buf->Unlock();
    }

    // -----------------------------------------------------------------------
    // TRIM THE CODEC'S PADDING, AND NOTHING ELSE.
    //
    // An mp3 is encoded in 1152-sample frames, so a file that is not a whole
    // number of frames long is padded with silence at both ends -- typically
    // 576 samples of encoder delay in front and up to 1151 behind. MF reads
    // the LAME/Xing tag and usually strips it, but "usually" is a click every
    // five minutes when it does not, and the crossfaded loop this plays was
    // baked precisely so that there would be no seam to hear.
    //
    // BOUNDED AT 100 ms, which is why this cannot eat anything real: it is
    // three times the worst padding an mp3 frame can carry and far short of
    // the quietest passage in an ambience bed. The threshold is a hair above
    // zero rather than a noise gate -- padding decodes to silence or to the
    // dither around it, not to quiet content.
    // -----------------------------------------------------------------------
    const size_t frames = out->size() / 2;
    const size_t kMaxTrim = 4800;  // 100 ms at 48 kHz
    const int kPadFloor = 8;       // about -72 dBFS
    size_t head = 0, tail = 0;
    while (head < kMaxTrim && head < frames && std::abs(int((*out)[head * 2])) <= kPadFloor &&
           std::abs(int((*out)[head * 2 + 1])) <= kPadFloor)
        ++head;
    while (tail < kMaxTrim && head + tail < frames &&
           std::abs(int((*out)[(frames - 1 - tail) * 2])) <= kPadFloor &&
           std::abs(int((*out)[(frames - 1 - tail) * 2 + 1])) <= kPadFloor)
        ++tail;
    if (tail) out->erase(out->end() - ptrdiff_t(tail * 2), out->end());
    if (head) out->erase(out->begin(), out->begin() + ptrdiff_t(head * 2));

    return !out->empty();
}

// ---------------------------------------------------------------------------
// Ambience -- one looping bed, one volume, and the smoothing on it.
// ---------------------------------------------------------------------------
class Ambience {
  public:
    Ambience() = default;
    ~Ambience() { stop(); }
    Ambience(const Ambience &) = delete;
    Ambience &operator=(const Ambience &) = delete;

    // Never fatal. Somebody without the asset, or on a machine with no audio
    // endpoint at all, gets a line on stderr and a silent forest -- an engine
    // that refused to start because a bird recording was missing would be a
    // worse bug than the missing birds.
    bool open(const std::string &path, float masterGain) {
        stop();
        master_ = masterGain;
        if (!decodeToPcm48Stereo(path, &pcm_)) return false;

        // THE BIRDS ARE TWENTY DB OVER THE WOOD, and the volume in
        // update() is one number for the whole bed -- it can only take the
        // wood away with them. Pulling the top of the range down here,
        // ONCE, at load, is the only place that distinction can be made:
        // by the time XAudio2 has the buffer there is a single gain left.
        // Measured on bird_ambience.mp3 this moves the loudest 50 ms
        // blocks down 5.7 dB and the median 0.3 -- see render/dynamics.h,
        // which carries the whole distribution and the reasoning.
        //
        // AFTER the padding trim and not before: the trim decides what is
        // silence by looking at raw sample values, and it should see the
        // ones the codec produced.
        tameLoudPeaks(&pcm_, Tame{});

        HRESULT hr = ::XAudio2Create(&xa_, 0, XAUDIO2_DEFAULT_PROCESSOR);
        if (FAILED(hr)) {
            std::fprintf(stderr, "v2: XAudio2Create failed (0x%08lx) -- no ambience\n",
                         (unsigned long)hr);
            pcm_.clear();
            return false;
        }
        hr = xa_->CreateMasteringVoice(&masterVoice_);
        if (FAILED(hr)) {
            std::fprintf(stderr, "v2: no audio endpoint (0x%08lx) -- no ambience\n",
                         (unsigned long)hr);
            stop();
            return false;
        }

        WAVEFORMATEX wfx{};
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = 2;
        wfx.nSamplesPerSec = 48000;
        wfx.wBitsPerSample = 16;
        wfx.nBlockAlign = WORD(wfx.nChannels * wfx.wBitsPerSample / 8);
        wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
        hr = xa_->CreateSourceVoice(&voice_, &wfx);
        if (FAILED(hr)) {
            std::fprintf(stderr, "v2: CreateSourceVoice failed (0x%08lx)\n", (unsigned long)hr);
            stop();
            return false;
        }

        // The buffer points straight into pcm_, which therefore may not move
        // or be freed while the voice lives -- it is a member for exactly that
        // reason, and stop() destroys the voice before it clears the vector.
        XAUDIO2_BUFFER b{};
        b.AudioBytes = UINT32(pcm_.size() * sizeof(int16_t));
        b.pAudioData = reinterpret_cast<const BYTE *>(pcm_.data());
        b.Flags = XAUDIO2_END_OF_STREAM;
        b.LoopCount = XAUDIO2_LOOP_INFINITE;  // LoopBegin/Length 0 == the whole buffer
        if (FAILED(voice_->SubmitSourceBuffer(&b))) {
            stop();
            return false;
        }

        voice_->SetVolume(0.0f);  // see the header: it runs from load, silently
        voice_->Start(0);
        std::printf("v2: ambience %s  (%.1f s, looping)\n", path.c_str(),
                    double(pcm_.size() / 2) / 48000.0);
        std::fflush(stdout);
        return true;
    }

    // target is 0..1 -- how much of a wood the listener is standing in.
    void update(float dt, float target) {
        if (!voice_) return;
        if (!(target > 0.0f)) target = 0.0f;  // also catches a NaN out of the terrain
        if (target > 1.0f) target = 1.0f;

        // The same first-order lag the player's speed and eye height use
        // (render/player.h), at a much longer time constant. THE RATE IS THE
        // WHOLE FEEL OF THIS: the stand-density field is a sixty-metre
        // feature, so at a walk its own value already takes ten seconds or so
        // to swing, and a fast follow would simply track that faithfully and
        // audibly. 0.45 -- about a two-second constant -- lands the fade a
        // little behind the trees, which is how walking out of a wood
        // actually sounds.
        level_ += (target - level_) * (1.0f - std::exp(-0.45f * dt));

        // Squared, because amplitude is not loudness: a linear ramp spends
        // most of its travel in a range the ear reads as already-full, so the
        // fade appears to finish early and then linger. x^2 is close enough to
        // a perceptual taper for a bed that is never in the foreground.
        voice_->SetVolume(level_ * level_ * master_);
    }

    void setMasterGain(float g) { master_ = g; }
    float masterGain() const { return master_; }
    float level() const { return level_; }
    bool active() const { return voice_ != nullptr; }

    // Ordered: the voice reads pcm_, so it dies first. Unlike MFShutdown --
    // which mfvideo.h deliberately never calls -- an audio device left open
    // past the window closing is audible, so this one really is torn down.
    void stop() {
        if (voice_) {
            voice_->Stop(0);
            voice_->DestroyVoice();
            voice_ = nullptr;
        }
        if (masterVoice_) {
            masterVoice_->DestroyVoice();
            masterVoice_ = nullptr;
        }
        xa_.Reset();
        pcm_.clear();
        pcm_.shrink_to_fit();
        level_ = 0.0f;
    }

  private:
    ComPtr<IXAudio2> xa_;
    IXAudio2MasteringVoice *masterVoice_ = nullptr;  // owned by xa_, destroyed by hand
    IXAudio2SourceVoice *voice_ = nullptr;
    std::vector<int16_t> pcm_;
    float level_ = 0.0f;
    float master_ = 1.0f;
};

}  // namespace vb
