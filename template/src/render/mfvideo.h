#pragma once
// ---------------------------------------------------------------------------
// mfvideo.h -- H.264 in and out of an mp4, through Media Foundation.
//
// Two classes, and they are two halves of the same choice:
//
//   VideoWriter   NV12 frames in, an mp4 out. Used by the live
//                 recorder and, at a different bitrate and GOP, by the export.
//   VideoReader   an mp4 in, decoded BGRA frames out, seekable by frame index.
//                 Nothing in the engine reads takes back today -- this is kept
//                 because the writer's constant-rate guarantee is only worth
//                 anything if something can verify it, and because it is the
//                 half any future editing or transcode step would need.
//
// WHY MEDIA FOUNDATION AND NOT NVENC. NVENC would be faster and would let the
// display texture reach the encoder without ever touching system memory, and
// v2 already has the CUDA interop that would make that work (src/gpu/cuda.h).
// It was still the wrong call here: it needs the Video Codec SDK headers
// vendored, it needs a hand-written mp4 muxer, and it needs a separate decoder
// binding of its own. MF ships with Windows, picks up the same NVIDIA
// hardware encoder MFT underneath on its own, writes the container, and hands
// back a matching decoder -- and it honours an explicitly stamped presentation
// time on every sample, which is the one property the recorder's whole design
// rests on (see the header of recorder.h).
//
// EVERYTHING BELOW STAMPS TIME FROM A FRAME INDEX. There is no call to a clock
// in this file. If you are ever tempted to add one, read rule 1 in recorder.h
// first.
// ---------------------------------------------------------------------------

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

// icodecapi.h before codecapi.h: the first declares the ICodecAPI interface,
// the second only the property GUIDs passed to it. Including codecapi.h alone
// compiles until the one line that actually asks for the interface, which is
// how this reads as "ICodecAPI: undeclared identifier" from a file that plainly
// includes the codec API header.
#include <icodecapi.h>

#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>   // std::llround, used below -- and only reached
                    // transitively today because Falcor gets there first
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace vb {

template <class T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

// ---------------------------------------------------------------------------
// MFStartup exactly once, whichever thread gets here first.
//
// MFStartup/MFShutdown are reference counted, so pairing them per user would
// also work; not shutting down at all is deliberate. A take can still be
// finalising on the encoder thread while the process is being torn down, and
// an MF that has been shut down under a live sink writer is a crash on exit
// for no gain -- the OS reclaims this at process end regardless.
// ---------------------------------------------------------------------------
inline bool mfInit() {
    static bool ok = [] {
        // MTA if this thread has no apartment yet. RPC_E_CHANGED_MODE means
        // something (GLFW, the shell) already made it an STA, which is fine:
        // every reader here is synchronous and the writer lives on a thread of
        // its own, so neither needs the multithreaded apartment.
        ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const HRESULT hr = ::MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(hr)) {
            std::fprintf(stderr, "v2: MFStartup failed (0x%08lx) -- no video recording\n",
                         (unsigned long)hr);
            return false;
        }
        return true;
    }();
    // Every thread that touches COM needs its own initialisation, and the
    // once-block above only ran on one of them.
    static thread_local bool perThread = [] {
        ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        return true;
    }();
    (void)perThread;
    return ok;
}

inline std::wstring widen(const std::string &s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}

// ---------------------------------------------------------------------------
// The presentation time of frame n, in Media Foundation's 100 ns units.
//
// THE ONE PIECE OF ARITHMETIC THIS WHOLE FEATURE TURNS ON. Written as a single
// multiply-then-divide of the ABSOLUTE index rather than as a running sum of
// per-frame durations, because a running sum of 166666 accumulates 20 ms of
// error a minute at 60 fps and the picture drifts against nothing in
// particular -- the classic way a "constant" frame rate stops being one.
//
// n * 10^7 stays inside an int64 for about 29,000 years of video, so there is
// no overflow to guard. The truncation is at most one 100 ns tick, which is
// one part in 1.67 million of a 60 fps frame.
// ---------------------------------------------------------------------------
inline int64_t slotPts(int64_t n, int fpsNum, int fpsDen) {
    const int64_t num = n * 10'000'000LL * int64_t(fpsDen);
    return (num + int64_t(fpsNum) / 2) / int64_t(fpsNum);
}

// ---------------------------------------------------------------------------
// VideoWriter
// ---------------------------------------------------------------------------
class VideoWriter {
  public:
    struct Config {
        std::string path;
        int width = 1920;
        int height = 1080;
        int fpsNum = 60;
        int fpsDen = 1;
        uint32_t bitrate = 25'000'000;
        // 0 means no audio track at all, which is what every caller got
        // before the recorder learned to keep the sound.
        int audioChannels = 0;
        // The endpoint's own rate, passed through rather than assumed. AAC
        // takes 44100 or 48000; anything else and the caller disables audio,
        // because writing 44100 samples and labelling them 48000 does not fail,
        // it just plays the take back nine per cent fast.
        int audioRate = 48000;
        // Distance between keyframes, in frames. Short for a recording that is
        // about to be scrubbed, long for an export that is about to be watched.
        int gopFrames = 60;
    };

    VideoWriter() = default;
    ~VideoWriter() { close(); }
    VideoWriter(const VideoWriter &) = delete;
    VideoWriter &operator=(const VideoWriter &) = delete;

    bool open(const Config &cfg) {
        if (!mfInit()) return false;
        close();
        cfg_ = cfg;

        ComPtr<IMFAttributes> attr;
        if (FAILED(::MFCreateAttributes(&attr, 4))) return false;
        // Let MF use the NVIDIA encoder MFT. Without this it silently picks the
        // Microsoft software encoder, which at 1440p60 is not merely slower --
        // it is slower than real time, so the ring in recorder.h drains and
        // every other frame is a held duplicate.
        attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        // The sink writer throttles WriteSample by BLOCKING when it thinks the
        // caller is ahead of the encoder. That is a sensible default for a
        // transcode and exactly wrong for a live capture: the recorder already
        // has a bounded queue and a drop policy, and a blocking WriteSample
        // would push that back pressure onto the render thread.
        attr->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        attr->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);

        // ── NO MF_MP4SINK_MOOV_BEFORE_MDAT, AND THAT IS MEASURED ──────────
        //
        // Faststart would be nice to have and this build asked for it. What MF
        // produced was a file with the moov in front, correct sample tables --
        // and NO mdat BOX AT ALL: ftyp, a uuid, moov, and then raw media bytes
        // with the chunk offsets pointing 16 bytes into them. ffprobe reads the
        // container happily (right frame count, right duration, uniform stts)
        // and then decodes exactly zero frames, because every "sample" it
        // fetches is arbitrary file content. A recorder that reports success
        // and writes 10 MB no player will open is the worst failure available
        // here, so the flag is gone.
        //
        // What is lost is nothing local: moov-at-the-end is the ordinary
        // layout, every player handles it, and it only costs anything when a
        // file is being streamed over HTTP before it has finished downloading.
        // The WebGPU game cared because its output WAS a download; this one
        // writes a file next to the exe.

        const std::wstring wpath = widen(cfg_.path);
        if (FAILED(::MFCreateSinkWriterFromURL(wpath.c_str(), nullptr, attr.Get(), &writer_)))
            return false;

        ComPtr<IMFMediaType> outType;
        if (FAILED(::MFCreateMediaType(&outType))) return closeFail();
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        outType->SetUINT32(MF_MT_AVG_BITRATE, cfg_.bitrate);
        outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        outType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
        ::MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, UINT32(cfg_.width),
                             UINT32(cfg_.height));
        ::MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, UINT32(cfg_.fpsNum),
                              UINT32(cfg_.fpsDen));
        ::MFSetAttributeRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        tagColour(outType.Get());
        if (FAILED(writer_->AddStream(outType.Get(), &stream_))) return closeFail();

        ComPtr<IMFMediaType> inType;
        if (FAILED(::MFCreateMediaType(&inType))) return closeFail();
        inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        // Contiguous NV12 -- the conversion shader writes exactly this, with no
        // padding, so the stride is the width.
        inType->SetUINT32(MF_MT_DEFAULT_STRIDE, UINT32(cfg_.width));
        ::MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, UINT32(cfg_.width),
                             UINT32(cfg_.height));
        ::MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, UINT32(cfg_.fpsNum),
                              UINT32(cfg_.fpsDen));
        ::MFSetAttributeRatio(inType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        tagColour(inType.Get());
        if (FAILED(writer_->SetInputMediaType(stream_, inType.Get(), nullptr))) return closeFail();

        // -- AND THE SOUND, IF THE TAKE IS BRINGING ANY ----------------------
        //
        // Added BEFORE BeginWriting, because a sink writer's streams are fixed
        // once writing starts -- there is no adding a track to a file already
        // being written, so the decision has to be made here, from the config.
        //
        // AAC AT 48 kHz, which is not a free choice: the AAC encoder MFT
        // accepts 44100 or 48000 and nothing else, and it wants 16-bit PCM in.
        // The tap gives floats at whatever the endpoint runs at, so the
        // recorder converts -- see writeAudio.
        //
        // A FAILURE HERE IS NOT FATAL. The take still gets its picture; it just
        // has no sound, which is what this engine did until now anyway.
        if (cfg_.audioChannels > 0) {
            ComPtr<IMFMediaType> aOut;
            if (SUCCEEDED(::MFCreateMediaType(&aOut))) {
                aOut->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                aOut->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
                aOut->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                aOut->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, UINT32(cfg_.audioRate));
                aOut->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, UINT32(cfg_.audioChannels));
                aOut->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 16000);  // 128 kbps
                if (SUCCEEDED(writer_->AddStream(aOut.Get(), &audioStream_))) {
                    ComPtr<IMFMediaType> aIn;
                    if (SUCCEEDED(::MFCreateMediaType(&aIn))) {
                        aIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                        aIn->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
                        aIn->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                        aIn->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, UINT32(cfg_.audioRate));
                        aIn->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, UINT32(cfg_.audioChannels));
                        aIn->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,
                                       UINT32(2 * cfg_.audioChannels));
                        aIn->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                                       UINT32(cfg_.audioRate * 2 * cfg_.audioChannels));
                        aIn->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
                        if (FAILED(writer_->SetInputMediaType(audioStream_, aIn.Get(), nullptr)))
                            audioStream_ = kNoStream;
                    } else {
                        audioStream_ = kNoStream;
                    }
                } else {
                    audioStream_ = kNoStream;
                }
            }
            if (audioStream_ == kNoStream)
                std::fprintf(stderr, "v2: no AAC encoder -- this take will be silent\n");
        }

        applyCodecOptions();

        if (FAILED(writer_->BeginWriting())) return closeFail();
        frameBytes_ = size_t(cfg_.width) * size_t(cfg_.height) * 3 / 2;
        open_ = true;
        return true;
    }

    bool isOpen() const { return open_; }
    bool hasAudio() const { return audioStream_ != kNoStream; }

    // -----------------------------------------------------------------------
    // One block of PCM at `frames` into the take, in the VIDEO's timeline.
    //
    // The presentation time is computed from a running frame COUNT rather than
    // from a clock, for exactly the reason the picture's is computed from a
    // slot: the two tracks have to be laid on one ruler or they drift. At 48
    // kHz a frame is 1/48000 of a second and the arithmetic is exact in 100 ns
    // units, so audio written from sample zero lines up with video written from
    // slot zero with nothing to correct.
    // -----------------------------------------------------------------------
    bool writeAudio(const int16_t *pcm, int frames, int channels, int64_t frameIndex) {
        if (audioStream_ == kNoStream || frames <= 0) return false;
        const DWORD bytes = DWORD(frames) * DWORD(channels) * 2u;
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(::MFCreateMemoryBuffer(bytes, &buf))) return false;
        BYTE *dst = nullptr;
        if (FAILED(buf->Lock(&dst, nullptr, nullptr))) return false;
        std::memcpy(dst, pcm, bytes);
        buf->Unlock();
        buf->SetCurrentLength(bytes);

        ComPtr<IMFSample> sample;
        if (FAILED(::MFCreateSample(&sample))) return false;
        if (FAILED(sample->AddBuffer(buf.Get()))) return false;
        const int64_t t0 = frameIndex * 10000000LL / cfg_.audioRate;
        const int64_t t1 = (frameIndex + frames) * 10000000LL / cfg_.audioRate;
        sample->SetSampleTime(t0);
        sample->SetSampleDuration(t1 - t0);
        return SUCCEEDED(writer_->WriteSample(audioStream_, sample.Get()));
    }
    bool hasLastFrame() const { return lastBuffer_ != nullptr; }

    // Write one picture at slot `slot`. The pixels are copied into a media
    // buffer that is kept alive afterwards, so repeatLast can re-present the
    // same picture at other times without another copy.
    bool writeFrame(const uint8_t *nv12, size_t bytes, int64_t slot) {
        if (!open_ || !nv12 || bytes < frameBytes_) return false;
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(::MFCreateMemoryBuffer(DWORD(frameBytes_), &buf))) return false;
        BYTE *dst = nullptr;
        if (FAILED(buf->Lock(&dst, nullptr, nullptr))) return false;
        std::memcpy(dst, nv12, frameBytes_);
        buf->Unlock();
        buf->SetCurrentLength(DWORD(frameBytes_));
        lastBuffer_ = buf;
        return present(buf.Get(), slot);
    }

    // The same picture again, at `count` consecutive slots starting at `first`.
    //
    // ONE BUFFER, SEVERAL SAMPLES. An IMFSample carries a time and a reference
    // to its buffer, so a held frame costs a small allocation and no pixel
    // copy at all -- which is what makes filling a hitch cheap enough to do
    // unconditionally rather than only when it looks affordable.
    bool repeatLast(int64_t first, int64_t count) {
        if (!open_ || !lastBuffer_ || count <= 0) return false;
        for (int64_t i = 0; i < count; ++i)
            if (!present(lastBuffer_.Get(), first + i)) return false;
        return true;
    }

    bool close() {
        bool ok = true;
        if (writer_ && open_) {
            const HRESULT hr = writer_->Finalize();
            ok = SUCCEEDED(hr);
            if (!ok)
                std::fprintf(stderr, "v2: sink writer Finalize failed (0x%08lx)\n",
                             (unsigned long)hr);
        }
        writer_.Reset();
        lastBuffer_.Reset();
        open_ = false;
        return ok;
    }

  private:
    bool closeFail() {
        writer_.Reset();
        open_ = false;
        return false;
    }

    // Said twice on purpose: the conversion shader writes limited-range BT.709
    // and the media type declares limited-range BT.709. A stream that carries
    // one and claims the other is the standard washed-out / crushed-blacks
    // recording, and it is entirely silent until someone watches the file.
    static void tagColour(IMFMediaType *t) {
        t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        t->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        t->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
        t->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
        t->SetUINT32(MF_MT_VIDEO_CHROMA_SITING,
                     MFVideoChromaSubsampling_MPEG2 | MFVideoChromaSubsampling_ProgressiveChroma);
    }

    // Every one of these is advisory: encoder MFTs differ in what they expose
    // and a refused property is not an error, so each is set and its result
    // dropped. Nothing here changes whether the file is correct -- only how
    // the bits are spent.
    void applyCodecOptions() {
        // Every one of the six below was checked to return S_OK on this
        // machine's encoder MFT -- worth knowing, because a refused codec
        // property is not an error and an unchecked SetValue would have let
        // the rate control, the GOP and the B-frame count all be silently
        // ignored while the file still came out.
        ComPtr<ICodecAPI> codec;
        if (FAILED(writer_->GetServiceForStream(stream_, GUID_NULL, IID_PPV_ARGS(&codec)))) return;
        auto setU32 = [&](const GUID &g, UINT32 v) {
            VARIANT var;
            ::VariantInit(&var);
            var.vt = VT_UI4;
            var.ulVal = v;
            codec->SetValue(&g, &var);
            ::VariantClear(&var);
        };
        // Unconstrained VBR rather than the default CBR. A forest is mostly
        // still trees and occasionally a fast pan; CBR spends the same bits on
        // both, which means the pan is where the quality goes.
        setU32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_UnconstrainedVBR);
        setU32(CODECAPI_AVEncCommonMeanBitRate, cfg_.bitrate);
        setU32(CODECAPI_AVEncMPVGOPSize, UINT32(std::max(1, cfg_.gopFrames)));
        // No B-frames. They would buy a few percent, and they reorder output:
        // an editor that seeks by presentation time through a stream with
        // reordering has to deal with a decode order that is not display
        // order, for a saving nobody watching a game capture will see.
        setU32(CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        setU32(CODECAPI_AVEncCommonQualityVsSpeed, 70);
        setU32(CODECAPI_AVEncVideoForceKeyFrame, 0);
    }

    bool present(IMFMediaBuffer *buf, int64_t slot) {
        ComPtr<IMFSample> sample;
        if (FAILED(::MFCreateSample(&sample))) return false;
        if (FAILED(sample->AddBuffer(buf))) return false;
        const int64_t t0 = slotPts(slot, cfg_.fpsNum, cfg_.fpsDen);
        const int64_t t1 = slotPts(slot + 1, cfg_.fpsNum, cfg_.fpsDen);
        sample->SetSampleTime(t0);
        sample->SetSampleDuration(t1 - t0);
        const HRESULT hr = writer_->WriteSample(stream_, sample.Get());
        if (FAILED(hr)) {
            std::fprintf(stderr, "v2: WriteSample failed (0x%08lx)\n", (unsigned long)hr);
            return false;
        }
        return true;
    }

    Config cfg_;
    // AAC's own rate. The encoder takes 44100 or 48000 and nothing else, and
    // 48000 is what every endpoint this runs on already uses, so choosing it
    // means the common case needs no resampling at all.
    static constexpr DWORD kNoStream = DWORD(-1);
    DWORD audioStream_ = kNoStream;

    ComPtr<IMFSinkWriter> writer_;
    ComPtr<IMFMediaBuffer> lastBuffer_;
    DWORD stream_ = 0;
    size_t frameBytes_ = 0;
    bool open_ = false;
};

// ---------------------------------------------------------------------------
// VideoReader -- decode an mp4 to BGRA, addressed by frame index.
//
// ADDRESSED BY FRAME INDEX, not by time, and that is only honest because the
// recorder writes constant-rate files. Frame n is at exactly slotPts(n), so
// "give me frame n" is a seek to that time and a read forward, with no
// tolerance window and no risk of landing a frame either side. An editor built
// on a variable-rate source could not do this, which is one more thing that
// falls out of the recorder's first rule.
// ---------------------------------------------------------------------------
class VideoReader {
  public:
    VideoReader() = default;
    ~VideoReader() { close(); }
    VideoReader(const VideoReader &) = delete;
    VideoReader &operator=(const VideoReader &) = delete;

    bool open(const std::string &path) {
        if (!mfInit()) return false;
        close();

        ComPtr<IMFAttributes> attr;
        if (FAILED(::MFCreateAttributes(&attr, 3))) return false;
        // Lets the reader insert a video processor so an RGB output type can be
        // asked for; without it only the decoder's native NV12 is on offer and
        // the conversion would have to be done here.
        attr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);

        const std::wstring wpath = widen(path);
        if (FAILED(::MFCreateSourceReaderFromURL(wpath.c_str(), attr.Get(), &reader_))) return false;

        reader_->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        reader_->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

        // Read the native type first: the size and the frame rate are wanted
        // whether or not the output type below is accepted.
        ComPtr<IMFMediaType> native;
        if (FAILED(reader_->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native)))
            return closeFail();
        UINT32 w = 0, h = 0, num = 0, den = 0;
        ::MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &w, &h);
        if (FAILED(::MFGetAttributeRatio(native.Get(), MF_MT_FRAME_RATE, &num, &den)) || !den) {
            num = 60;
            den = 1;
        }
        width_ = int(w);
        height_ = int(h);
        fpsNum_ = int(num);
        fpsDen_ = int(den);
        if (width_ <= 0 || height_ <= 0) return closeFail();

        ComPtr<IMFMediaType> out;
        if (FAILED(::MFCreateMediaType(&out))) return closeFail();
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (FAILED(reader_->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr,
                                                out.Get())))
            return closeFail();

        // The stride, and its SIGN. RGB32 out of a video processor is very
        // often bottom-up, and a bottom-up frame copied as if it were top-down
        // is an upside-down preview that looks like a bug in the recorder
        // rather than in the reader.
        ComPtr<IMFMediaType> cur;
        stride_ = width_ * 4;
        if (SUCCEEDED(reader_->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur))) {
            UINT32 s = 0;
            if (SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &s)))
                stride_ = int(int32_t(s));
            else {
                LONG ls = 0;
                if (SUCCEEDED(::MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1,
                                                               UINT32(width_), &ls)))
                    stride_ = int(ls);
            }
        }

        frames_ = probeFrameCount();
        path_ = path;
        cursor_ = -1;
        return true;
    }

    void close() {
        reader_.Reset();
        path_.clear();
        width_ = height_ = 0;
        frames_ = 0;
        cursor_ = -1;
    }

    bool isOpen() const { return reader_ != nullptr; }
    int width() const { return width_; }
    int height() const { return height_; }
    int fpsNum() const { return fpsNum_; }
    int fpsDen() const { return fpsDen_; }
    double fps() const { return fpsDen_ ? double(fpsNum_) / double(fpsDen_) : 60.0; }
    int64_t frames() const { return frames_; }
    double seconds() const { return fps() > 0.0 ? double(frames_) / fps() : 0.0; }
    const std::string &path() const { return path_; }

    // Decode frame `index` into `rgba` (BGRA byte order, top-down, tightly
    // packed at width*4). Returns false past the end.
    //
    // Reading FORWARD from the current position is the common case -- playback
    // and export both do it -- and costs one ReadSample. A seek is only issued
    // when the wanted frame is behind the cursor or far ahead of it, because
    // SetCurrentPosition flushes the decoder and lands on the preceding
    // keyframe: at a 1 s GOP, stepping forward through 30 frames is cheaper
    // than seeking and re-decoding up to 60.
    bool readFrame(int64_t index, std::vector<uint8_t> &rgba) {
        if (!reader_ || index < 0) return false;
        if (frames_ > 0 && index >= frames_) return false;

        if (cursor_ < 0 || index < cursor_ || index > cursor_ + kSeekAhead) {
            if (!seekTo(index)) return false;
        }

        for (;;) {
            DWORD flags = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> sample;
            DWORD actual = 0;
            const HRESULT hr = reader_->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &actual,
                                                   &flags, &ts, &sample);
            if (FAILED(hr)) return false;
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                cursor_ = -1;
                return false;
            }
            if (!sample) continue;  // a stream tick, not a picture

            // Which frame this is, from its time and the file's own rate.
            // Rounded rather than truncated: the muxer's timescale and MF's
            // 100 ns units do not divide evenly at 60 fps, so a frame's time
            // can land a tick below its exact slot.
            const int64_t got = int64_t(std::llround(double(ts) * fps() / 10'000'000.0));
            cursor_ = got;
            if (got < index) continue;  // still catching up from the keyframe

            return copyOut(sample.Get(), rgba);
        }
    }

  private:
    static constexpr int64_t kSeekAhead = 90;  // ~1.5 s at 60: about one GOP

    bool closeFail() {
        close();
        return false;
    }

    bool seekTo(int64_t index) {
        PROPVARIANT var;
        ::PropVariantInit(&var);
        var.vt = VT_I8;
        var.hVal.QuadPart = slotPts(index, fpsNum_, fpsDen_);
        const HRESULT hr = reader_->SetCurrentPosition(GUID_NULL, var);
        ::PropVariantClear(&var);
        if (FAILED(hr)) return false;
        cursor_ = -1;
        return true;
    }

    bool copyOut(IMFSample *sample, std::vector<uint8_t> &rgba) {
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->ConvertToContiguousBuffer(&buf))) return false;

        const size_t rowBytes = size_t(width_) * 4;
        rgba.resize(rowBytes * size_t(height_));

        // Lock2D when the buffer offers it: it hands back a pointer to the TOP
        // row plus a signed pitch, which is the only description of a bottom-up
        // frame that is not ambiguous.
        ComPtr<IMF2DBuffer> two;
        if (SUCCEEDED(buf.As(&two))) {
            BYTE *scan0 = nullptr;
            LONG pitch = 0;
            if (SUCCEEDED(two->Lock2D(&scan0, &pitch))) {
                for (int y = 0; y < height_; ++y)
                    std::memcpy(rgba.data() + rowBytes * size_t(y), scan0 + ptrdiff_t(pitch) * y,
                                rowBytes);
                two->Unlock2D();
                return true;
            }
        }

        BYTE *p = nullptr;
        DWORD len = 0;
        if (FAILED(buf->Lock(&p, nullptr, &len))) return false;
        const int pitch = stride_;
        const size_t need = size_t(std::abs(pitch)) * size_t(height_);
        if (len < need) {
            buf->Unlock();
            return false;
        }
        // A negative stride means the buffer holds the picture bottom-up, so
        // the first row in memory is the LAST row of the image.
        const BYTE *top = pitch < 0 ? p + size_t(-pitch) * size_t(height_ - 1) : p;
        for (int y = 0; y < height_; ++y)
            std::memcpy(rgba.data() + rowBytes * size_t(y), top + ptrdiff_t(pitch) * y, rowBytes);
        buf->Unlock();
        return true;
    }

    // How long the file is. Asked of the container rather than counted by
    // decoding it: MF_PD_DURATION comes out of the moov, so this is instant
    // even on a long take.
    int64_t probeFrameCount() {
        PROPVARIANT var;
        ::PropVariantInit(&var);
        int64_t n = 0;
        if (SUCCEEDED(reader_->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE,
                                                        MF_PD_DURATION, &var)) &&
            var.vt == VT_UI8) {
            const double secs = double(var.uhVal.QuadPart) / 10'000'000.0;
            n = int64_t(std::llround(secs * fps()));
        }
        ::PropVariantClear(&var);
        return std::max<int64_t>(n, 0);
    }

    ComPtr<IMFSourceReader> reader_;
    std::string path_;
    int width_ = 0, height_ = 0;
    int fpsNum_ = 60, fpsDen_ = 1;
    int stride_ = 0;
    int64_t frames_ = 0;
    int64_t cursor_ = -1;  // index of the last frame handed out, -1 after a seek
};

}  // namespace vb
