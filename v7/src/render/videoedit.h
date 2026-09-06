#pragma once
// ---------------------------------------------------------------------------
// videoedit.h -- the panel that opens when a take ends.
//
// A take is one file. An EDIT is an ordered list of ranges into that file, so
// trimming, splitting, deleting and reordering are all list operations and
// none of them touch a pixel until export. That is the whole data model:
//
//     std::vector<Clip> clips_;   // Clip = [in, out) in SOURCE frame numbers
//
// ═══════════════════════════════════════════════════════════════════════════
// WHY THE EXPORT CANNOT STUTTER, WHICH IS A DIFFERENT ARGUMENT FROM THE
// RECORDER'S
// ═══════════════════════════════════════════════════════════════════════════
//
// The WebGPU game exported by PLAYING the recording back in real time into a
// second recorder and capturing whatever the compositor presented. Its own
// note on that (ui/video-editor.js) is blunt about the consequence:
//
//     The export replays this recording in real time and captures presented
//     frames, so any frame the decoder cannot present in time is lost from the
//     file for good. VP8 at this resolution is software-decoded and could only
//     sustain ~46 fps of a 58 fps recording.
//
// So the export was a race, and the file was whatever survived it. Here it is
// not a race at all. Output frame n is DECODED BY INDEX from the source,
// converted, and encoded at slot n -- as fast or as slowly as the machine
// manages, with no clock involved anywhere. There is no mechanism by which a
// frame can be missing, and the output is constant rate for the same reason
// the recording is: every slot is written exactly once, and its presentation
// time is computed from its index (slotPts, mfvideo.h).
//
// It also means the export is allowed to be slow, which is what lets it read
// each frame back synchronously instead of running the fenced ring the live
// recorder needs. A stall during export costs seconds off a progress bar; a
// stall during capture costs frames out of the game.
// ---------------------------------------------------------------------------

#include <Falcor.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "render/mfvideo.h"
#include "render/recorder.h"

// For the "show the take" button only. After mfvideo.h, which is what brings
// windows.h in with WIN32_LEAN_AND_MEAN -- that macro is exactly what leaves
// shellapi.h out of it.
#include <shellapi.h>

namespace vb {

// ---------------------------------------------------------------------------
// A DRAW-LIST COLOUR, GIVEN AS THE COLOUR YOU WANT TO SEE.
//
// MEASURED, because it is invisible until you compare a screenshot against the
// constants that produced it: Falcor's ImGui backend writes vertex colours
// straight into a BGRA8UnormSrgb swapchain, so the hardware applies the sRGB
// transfer function to them on the way out and every colour comes back LIFTED.
// A near-black (14,18,25) trough renders as (66,75,88); a dark slate clip
// (46,86,122) renders as (118,157,184). Sampled off a --shot-ui capture, the
// rendered values matched srgbEncode(c/255) to the byte on all three.
//
// The consequence is not "slightly washed out", it is that DARKS COMPRESS: the
// gap between a trough and the block sitting in it, and between that block and
// its highlighted grab handle, all collapse towards white, and the timeline
// reads as one flat pale bar. So the palette below is written as the sRGB
// colour that should appear on screen, and inverted through here.
//
// The window chrome is deliberately NOT put through this. The panel background
// and the text colours were matched against the settings menu, which uses
// ui::rgb() in app.h and therefore lives in the same lifted space; making only
// this panel "correct" would make it the one that looks out of place.
// ---------------------------------------------------------------------------
inline ImU32 tlCol(int r, int g, int b, int a = 255) {
    auto lin = [](int v) {
        const float c = float(v) / 255.0f;
        const float l = c <= 0.04045f ? c * (1.0f / 12.92f)
                                      : std::pow((c + 0.055f) * (1.0f / 1.055f), 2.4f);
        return int(l * 255.0f + 0.5f);
    };
    return IM_COL32(lin(r), lin(g), lin(b), a);
}

// A range of the source take, in source frame numbers. Half open.
struct Clip {
    int64_t in = 0;
    int64_t out = 0;
    int64_t frames() const { return out > in ? out - in : 0; }
};

class VideoEditor {
  public:
    // Longest side the export offers. Anything above the source's own size is
    // hidden -- upscaling a capture is a bigger file and not a better picture.
    struct SizeChoice {
        const char *label;
        int height;  // 0 = the take's own size
    };

    void init(Falcor::ref<Falcor::Device> device, Nv12Convert *conv) {
        device_ = device;
        conv_ = conv;
    }

    bool open() const { return open_; }
    bool exporting() const { return exp_.active; }

    // True while the panel wants the mouse and the game should not be driven.
    bool modal() const { return open_; }

    // -----------------------------------------------------------------------
    // Take a finished recording and open on it.
    // -----------------------------------------------------------------------
    bool load(const Take &take) {
        close();
        if (!take.valid()) return false;
        if (!reader_.open(take.path)) {
            std::fprintf(stderr, "v7: could not open %s for editing\n", take.path.c_str());
            return false;
        }
        take_ = take;
        // Trust the FILE over the recorder's own count where they disagree:
        // the reader read the duration out of the container, which is what any
        // player will do too.
        if (reader_.frames() > 0) take_.frames = reader_.frames();
        take_.fpsNum = reader_.fpsNum();
        take_.fpsDen = reader_.fpsDen();
        // ...and the SIZE from the file too. The recorder's own numbers are
        // right, but this panel is one `load` away from being pointed at a
        // take from a previous session, and everything below -- the preview
        // aspect, the export ladder, the size estimate -- reads these.
        if (reader_.width() > 0 && reader_.height() > 0) {
            take_.width = reader_.width();
            take_.height = reader_.height();
        }

        clips_.clear();
        clips_.push_back(Clip{0, take_.frames});
        sel_ = 0;
        playhead_ = 0;
        shown_ = -1;
        playing_ = false;
        status_.clear();
        open_ = true;
        return true;
    }

    void close() {
        cancelExport();
        reader_.close();
        clips_.clear();
        preview_ = nullptr;
        srcTex_ = nullptr;
        shown_ = -1;
        playing_ = false;
        open_ = false;
    }

    // -----------------------------------------------------------------------
    // Per frame, on the render thread. Decodes what the preview needs and
    // advances an export if one is running.
    // -----------------------------------------------------------------------
    void tick(Falcor::RenderContext *ctx, double now) {
        if (!open_) return;
        // ONE CLOCK, and it is the app's. restartPlayClock is called from the
        // UI, which does not get handed `now`, so it reads this -- mixing
        // ImGui::GetTime() in there made the first play after a scrub jump by
        // however far apart the two origins happened to be.
        now_ = now;

        if (playing_ && !exp_.active) {
            const int64_t total = totalFrames();
            const int64_t t = playFrom_ + int64_t((now - playStart_) * fps());
            if (t >= total) {
                if (loop_ && total > 0) {
                    playFrom_ = 0;
                    playStart_ = now;
                    playhead_ = 0;
                } else {
                    playhead_ = total > 0 ? total - 1 : 0;
                    playing_ = false;
                }
            } else {
                playhead_ = t;
            }
        }

        if (exp_.active) {
            stepExport(ctx);
            return;
        }

        // The preview follows whichever frame the panel most recently asked
        // for -- the playhead normally, the handle being dragged while one is.
        const int64_t want = previewTarget();
        if (want != shown_ && want >= 0) {
            if (decodeTo(ctx, want, preview_)) shown_ = want;
        }
    }

    // -----------------------------------------------------------------------
    // The panel.
    // -----------------------------------------------------------------------
    void draw(float fbW, float fbH, float scale) {
        if (!open_) return;

        const float w = std::min(fbW - 40.0f, std::max(760.0f, fbW * 0.62f));
        ImGui::SetNextWindowPos(ImVec2((fbW - w) * 0.5f, fbH * 0.04f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(w, 0.0f), ImGuiCond_Always);

        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.055f, 0.075f, 0.105f, 0.94f));
        ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(0.055f, 0.075f, 0.105f, 0.94f));
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.09f, 0.12f, 0.16f, 0.96f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);

        bool stayOpen = true;
        if (ImGui::Begin("video##v7", &stayOpen,
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::SetWindowFontScale(scale);
            if (exp_.active)
                drawExporting();
            else
                drawEditor(w);
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(3);

        if (!stayOpen && !exp_.active) close();
    }

  private:
    // ── the edit ──────────────────────────────────────────────────────────
    double fps() const { return take_.fps(); }

    int64_t totalFrames() const {
        int64_t n = 0;
        for (const Clip &c : clips_) n += c.frames();
        return n;
    }

    // Output frame -> source frame. -1 past the end.
    int64_t sourceOf(int64_t outFrame) const {
        if (outFrame < 0) return -1;
        int64_t acc = 0;
        for (const Clip &c : clips_) {
            const int64_t n = c.frames();
            if (outFrame < acc + n) return c.in + (outFrame - acc);
            acc += n;
        }
        return -1;
    }

    // Which clip an output frame falls in, and where that clip starts.
    int clipAt(int64_t outFrame, int64_t *clipStart = nullptr) const {
        int64_t acc = 0;
        for (size_t i = 0; i < clips_.size(); ++i) {
            const int64_t n = clips_[i].frames();
            if (outFrame < acc + n) {
                if (clipStart) *clipStart = acc;
                return int(i);
            }
            acc += n;
        }
        if (clipStart) *clipStart = acc;
        return clips_.empty() ? -1 : int(clips_.size()) - 1;
    }

    int64_t clipStartOut(int index) const {
        int64_t acc = 0;
        for (int i = 0; i < index && i < int(clips_.size()); ++i) acc += clips_[i].frames();
        return acc;
    }

    int64_t previewTarget() const {
        if (dragPreview_ >= 0) return dragPreview_;
        return sourceOf(std::min(playhead_, std::max<int64_t>(0, totalFrames() - 1)));
    }

    void clampPlayhead() {
        const int64_t total = totalFrames();
        playhead_ = std::clamp<int64_t>(playhead_, 0, total > 0 ? total - 1 : 0);
    }

    // ── decode + upload ───────────────────────────────────────────────────
    bool decodeTo(Falcor::RenderContext *ctx, int64_t srcFrame, Falcor::ref<Falcor::Texture> &tex) {
        if (srcFrame < 0) return false;
        if (!reader_.readFrame(srcFrame, rgba_)) return false;
        const uint32_t w = uint32_t(reader_.width()), h = uint32_t(reader_.height());
        if (!tex || tex->getWidth() != w || tex->getHeight() != h) {
            // BGRA8Unorm, not its sRGB sibling: the decoded bytes ARE the
            // gamma-encoded values, and a _Srgb view would have the hardware
            // linearise them on read. The preview wants them shown as-is and
            // the conversion shader is told they are already curved
            // (gDecodeSrgb), so both paths agree that no view-level transfer
            // function belongs here.
            tex = device_->createTexture2D(w, h, Falcor::ResourceFormat::BGRA8Unorm, 1, 1, nullptr,
                                           Falcor::ResourceBindFlags::ShaderResource);
            tex->setName("v7::videoedit::frame");
        }
        ctx->updateTextureData(tex.get(), rgba_.data());
        return true;
    }

    // ── the editor UI ─────────────────────────────────────────────────────
    void drawEditor(float panelW) {
        const int64_t total = totalFrames();
        clampPlayhead();

        // -- preview ------------------------------------------------------
        const float aspect = take_.height > 0 ? float(take_.width) / float(take_.height) : 1.777f;
        const float imgW = panelW - 32.0f;
        const float imgH = imgW / std::max(0.1f, aspect);
        // Drawn into the window's draw list rather than through
        // Gui::Widgets::image: that helper sizes from the window and this
        // panel wants an exact rectangle, and the timeline below then shares
        // the same coordinate space. The (ImTextureID)Texture* convention is
        // Falcor's own -- its ImGui backend binds pCmd->TextureId as an SRV.
        {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImDrawList *dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, ImVec2(p.x + imgW, p.y + imgH), tlCol(9, 11, 16), 3.0f);
            if (preview_)
                dl->AddImage(reinterpret_cast<ImTextureID>(preview_.get()), p,
                             ImVec2(p.x + imgW, p.y + imgH));
            dl->AddRect(p, ImVec2(p.x + imgW, p.y + imgH), tlCol(56, 72, 96), 3.0f);
            ImGui::Dummy(ImVec2(imgW, imgH));
        }

        // -- transport ----------------------------------------------------
        ImGui::Spacing();
        if (ImGui::Button(playing_ ? "  pause  " : "  play  ")) togglePlay();
        ImGui::SameLine();
        if (ImGui::Button(" |< ")) {
            playhead_ = 0;
            restartPlayClock();
        }
        ImGui::SameLine();
        if (ImGui::Button(" < ")) step(-1);
        ImGui::SameLine();
        if (ImGui::Button(" > ")) step(1);
        ImGui::SameLine();
        if (ImGui::Button(" >| ")) {
            playhead_ = total > 0 ? total - 1 : 0;
            restartPlayClock();
        }
        ImGui::SameLine();
        ImGui::Checkbox("loop", &loop_);
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.62f, 0.69f, 0.78f, 1.0f), "%s / %s   %lld frames @ %.3g fps",
                           timecode(double(playhead_) / fps()).c_str(),
                           timecode(double(total) / fps()).c_str(), (long long)total, fps());

        // -- timeline -----------------------------------------------------
        ImGui::Spacing();
        drawTimeline(imgW);

        // -- clip operations ----------------------------------------------
        ImGui::Spacing();
        const bool haveSel = sel_ >= 0 && sel_ < int(clips_.size());
        if (ImGui::Button(" split here ")) splitAtPlayhead();
        ImGui::SameLine();
        ImGui::BeginDisabled(!haveSel || clips_.size() <= 1);
        if (ImGui::Button(" delete clip ")) deleteSelected();
        ImGui::SameLine();
        if (ImGui::Button(" move left ")) moveSelected(-1);
        ImGui::SameLine();
        if (ImGui::Button(" move right ")) moveSelected(1);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(" reset edit ")) {
            clips_.assign(1, Clip{0, take_.frames});
            sel_ = 0;
            playhead_ = 0;
        }

        if (haveSel) {
            Clip &c = clips_[size_t(sel_)];
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.99f, 0.84f, 0.47f, 1.0f), "clip %d of %d", sel_ + 1,
                               int(clips_.size()));
            // Frame-exact trim, because dragging a handle over a 700 px
            // timeline for a two-minute take moves five frames a pixel.
            int inF = int(c.in), outF = int(c.out);
            ImGui::PushItemWidth(imgW * 0.42f);
            if (ImGui::SliderInt("in", &inF, 0, int(take_.frames) - 1)) {
                c.in = std::clamp<int64_t>(inF, 0, c.out - 1);
                dragPreview_ = c.in;
            }
            if (ImGui::IsItemDeactivated()) dragPreview_ = -1;
            ImGui::SameLine();
            if (ImGui::SliderInt("out", &outF, 1, int(take_.frames))) {
                c.out = std::clamp<int64_t>(outF, c.in + 1, take_.frames);
                dragPreview_ = c.out - 1;
            }
            if (ImGui::IsItemDeactivated()) dragPreview_ = -1;
            ImGui::PopItemWidth();
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.62f, 0.69f, 0.78f, 1.0f), "%s",
                               timecode(double(c.frames()) / fps()).c_str());
        }

        // -- export -------------------------------------------------------
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::PushItemWidth(imgW * 0.28f);
        const char *sizes[] = {"same as the take", "1440p", "1080p", "720p"};
        ImGui::Combo("size", &sizeChoice_, sizes, IM_ARRAYSIZE(sizes));
        ImGui::SameLine();
        ImGui::SliderFloat("quality", &bpp_, 0.04f, 0.24f, "%.3f bpp");
        ImGui::PopItemWidth();
        ImGui::SameLine();
        int ew = 0, eh = 0;
        exportDims(ew, eh);
        const double secs = double(total) / fps();
        const double rate = exportRate(ew, eh);
        ImGui::TextColored(ImVec4(0.62f, 0.69f, 0.78f, 1.0f), "%dx%d,  ~%.0f MB",
                           ew, eh, rate * secs / 8.0 / 1.0e6);

        ImGui::Spacing();
        ImGui::BeginDisabled(total <= 0);
        if (ImGui::Button("  export mp4  ")) startExport();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("  show the take  ")) revealInExplorer(take_.path);
        ImGui::SameLine();
        if (ImGui::Button("  discard take  ")) discard();
        ImGui::SameLine();
        if (ImGui::Button("  close  ")) close();

        if (!status_.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.49f, 0.86f, 1.0f, 1.0f), "%s", status_.c_str());
        }
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.44f, 0.49f, 0.57f, 1.0f),
                           "take: %s  (%dx%d, %.1f s)", take_.path.c_str(), take_.width,
                           take_.height, take_.seconds());
    }

    // The timeline: a scrub ruler over a row of clips. Drawn by hand rather
    // than assembled from widgets, because what it has to express -- several
    // ranges laid end to end, with a playhead that belongs to the whole strip
    // and handles that belong to one clip -- is not any widget's shape.
    void drawTimeline(float width) {
        const int64_t total = std::max<int64_t>(1, totalFrames());
        // ── SIZED IN EMS, NOT IN PIXELS ───────────────────────────────────
        // These were fixed pixel counts, which is only ever right on the one
        // display they were tried on: at 3820x1990 the whole timeline came out
        // 60 px tall beneath 28 px text -- a strip too thin to aim a trim
        // handle at. GetFontSize() inside a window already carries the window's
        // font scale, so deriving from it tracks the rest of the interface at
        // any DPI without this panel needing to know what the DPI is.
        const float em = std::max(8.0f, ImGui::GetFontSize());
        const float rulerH = em * 0.75f;
        const float clipsH = em * 1.90f;
        const float handle = em * 0.34f;
        const float pad = em * 0.14f;
        const float h = rulerH + clipsH + pad;

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##timeline", ImVec2(width, h));
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();
        ImDrawList *dl = ImGui::GetWindowDrawList();

        const float y0 = origin.y;
        const float clipsY = y0 + rulerH + pad;
        const float clipsY1 = clipsY + clipsH;
        auto xOf = [&](int64_t frame) {
            return origin.x + width * float(double(frame) / double(total));
        };
        auto frameOf = [&](float x) {
            return int64_t(std::llround(double((x - origin.x) / width) * double(total)));
        };

        dl->AddRectFilled(ImVec2(origin.x, y0), ImVec2(origin.x + width, y0 + rulerH),
                          tlCol(26, 32, 44), 2.0f);
        dl->AddRectFilled(ImVec2(origin.x, clipsY), ImVec2(origin.x + width, clipsY1),
                          tlCol(15, 19, 27), 2.0f);

        // A tick a second, and a brighter one every ten.
        for (int64_t s = 0; s * int64_t(fps()) < total; ++s) {
            const float x = xOf(s * int64_t(fps()));
            const bool ten = (s % 10) == 0;
            dl->AddLine(ImVec2(x, y0 + rulerH * (ten ? 0.18f : 0.50f)), ImVec2(x, y0 + rulerH),
                        ten ? tlCol(132, 148, 170) : tlCol(72, 86, 106));
        }

        // The clips.
        int64_t acc = 0;
        for (size_t i = 0; i < clips_.size(); ++i) {
            const int64_t n = clips_[i].frames();
            const float xa = xOf(acc), xb = xOf(acc + n);
            const bool isSel = int(i) == sel_;
            const ImU32 fill = isSel ? tlCol(50, 92, 132) : tlCol(34, 48, 66);
            dl->AddRectFilled(ImVec2(xa + 1.0f, clipsY + pad), ImVec2(xb - 1.0f, clipsY1 - pad),
                              fill, 3.0f);
            dl->AddRect(ImVec2(xa + 1.0f, clipsY + pad), ImVec2(xb - 1.0f, clipsY1 - pad),
                        isSel ? tlCol(126, 220, 255) : tlCol(62, 82, 106), 3.0f);
            if (isSel) {
                // Grab handles, only on the selected clip: a strip of eight
                // clips all showing handles is a strip of pixels nobody can
                // hit the one they meant.
                dl->AddRectFilled(ImVec2(xa + 1.0f, clipsY + pad),
                                  ImVec2(xa + handle, clipsY1 - pad),
                                  tlCol(126, 220, 255, 230), 3.0f);
                dl->AddRectFilled(ImVec2(xb - handle, clipsY + pad),
                                  ImVec2(xb - 1.0f, clipsY1 - pad),
                                  tlCol(126, 220, 255, 230), 3.0f);
            }
            if (xb - xa > em * 2.6f) {
                char lbl[32];
                std::snprintf(lbl, sizeof(lbl), "%.1fs", double(n) / fps());
                dl->AddText(ImVec2(xa + em * 0.3f, clipsY + (clipsH - em) * 0.5f),
                            tlCol(214, 228, 244), lbl);
            }
            acc += n;
        }

        // The playhead, over everything.
        const float px = xOf(playhead_);
        dl->AddLine(ImVec2(px, y0), ImVec2(px, clipsY1), tlCol(255, 206, 96), em * 0.09f);
        dl->AddTriangleFilled(ImVec2(px - em * 0.26f, y0), ImVec2(px + em * 0.26f, y0),
                              ImVec2(px, y0 + em * 0.36f), tlCol(255, 206, 96));

        // -- interaction --------------------------------------------------
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        if (ImGui::IsItemActivated()) {
            drag_ = DragKind::None;
            if (mouse.y >= clipsY && sel_ >= 0 && sel_ < int(clips_.size())) {
                const int64_t s = clipStartOut(sel_);
                const float xa = xOf(s), xb = xOf(s + clips_[size_t(sel_)].frames());
                if (std::abs(mouse.x - xa) <= handle)
                    drag_ = DragKind::TrimIn;
                else if (std::abs(mouse.x - xb) <= handle)
                    drag_ = DragKind::TrimOut;
            }
            if (drag_ == DragKind::None) {
                if (mouse.y >= clipsY) {
                    const int hit = clipAt(std::clamp<int64_t>(frameOf(mouse.x), 0, total - 1));
                    if (hit >= 0) sel_ = hit;
                }
                drag_ = DragKind::Scrub;
            }
        }

        if (active) {
            const int64_t at = std::clamp<int64_t>(frameOf(mouse.x), 0, total - 1);
            switch (drag_) {
            case DragKind::Scrub:
                playhead_ = at;
                playing_ = false;
                break;
            case DragKind::TrimIn:
            case DragKind::TrimOut: {
                Clip &c = clips_[size_t(sel_)];
                const int64_t start = clipStartOut(sel_);
                // The drag is in OUTPUT frames; a trim moves a SOURCE edge.
                // The offset between them is fixed for this clip while the
                // drag lasts, which is what makes the handle track the cursor
                // instead of running away from it as the clip resizes.
                const int64_t delta = at - (drag_ == DragKind::TrimIn ? start
                                                                      : start + c.frames());
                if (drag_ == DragKind::TrimIn)
                    c.in = std::clamp<int64_t>(c.in + delta, 0, c.out - 1);
                else
                    c.out = std::clamp<int64_t>(c.out + delta, c.in + 1, take_.frames);
                dragPreview_ = (drag_ == DragKind::TrimIn) ? c.in : c.out - 1;
                break;
            }
            default:
                break;
            }
        } else if (drag_ != DragKind::None) {
            drag_ = DragKind::None;
            dragPreview_ = -1;
            clampPlayhead();
        }

        if (hovered && !active) {
            const int64_t at = std::clamp<int64_t>(frameOf(mouse.x), 0, total - 1);
            ImGui::SetTooltip("%s", timecode(double(at) / fps()).c_str());
        }
    }

    void drawExporting() {
        const float frac = exp_.total > 0 ? float(double(exp_.frame) / double(exp_.total)) : 0.0f;
        ImGui::TextColored(ImVec4(0.99f, 0.84f, 0.47f, 1.0f), "exporting %s", exp_.path.c_str());
        ImGui::Spacing();
        ImGui::ProgressBar(frac, ImVec2(520.0f, 0.0f));
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.62f, 0.69f, 0.78f, 1.0f), "%lld / %lld frames  --  %dx%d",
                           (long long)exp_.frame, (long long)exp_.total, exp_.w, exp_.h);
        ImGui::Spacing();
        if (ImGui::Button("  cancel  ")) cancelExport();
    }

    // ── transport helpers ─────────────────────────────────────────────────
    void togglePlay() {
        playing_ = !playing_;
        restartPlayClock();
        // Restarting from the end would show one frame and stop, which reads
        // as a button that does nothing.
        if (playing_ && playhead_ >= totalFrames() - 1) {
            playhead_ = 0;
            playFrom_ = 0;
        }
    }

    void restartPlayClock() {
        playFrom_ = playhead_;
        playStart_ = now_;
    }

    void step(int64_t d) {
        playing_ = false;
        playhead_ = std::clamp<int64_t>(playhead_ + d, 0, std::max<int64_t>(0, totalFrames() - 1));
    }

    // ── edit operations ───────────────────────────────────────────────────
    void splitAtPlayhead() {
        int64_t start = 0;
        const int i = clipAt(playhead_, &start);
        if (i < 0) return;
        Clip &c = clips_[size_t(i)];
        const int64_t at = c.in + (playhead_ - start);
        if (at <= c.in || at >= c.out) return;  // a split at an edge is a no-op, not two clips
        const Clip right{at, c.out};
        c.out = at;
        clips_.insert(clips_.begin() + i + 1, right);
        sel_ = i + 1;
    }

    void deleteSelected() {
        if (sel_ < 0 || sel_ >= int(clips_.size()) || clips_.size() <= 1) return;
        clips_.erase(clips_.begin() + sel_);
        sel_ = std::min(sel_, int(clips_.size()) - 1);
        clampPlayhead();
    }

    void moveSelected(int dir) {
        const int j = sel_ + dir;
        if (sel_ < 0 || j < 0 || j >= int(clips_.size())) return;
        std::swap(clips_[size_t(sel_)], clips_[size_t(j)]);
        sel_ = j;
    }

    void discard() {
        const std::string p = take_.path;
        close();
        if (!p.empty()) std::remove(p.c_str());
    }

    // ── export ────────────────────────────────────────────────────────────
    void exportDims(int &w, int &h) const {
        const int targets[] = {0, 1440, 1080, 720};
        const int want = targets[std::clamp(sizeChoice_, 0, 3)];
        w = take_.width;
        h = take_.height;
        // Never up: a bigger file of the same picture.
        if (want > 0 && want < h) {
            w = int(std::lround(double(take_.width) * double(want) / double(take_.height)));
            h = want;
        }
        alignCaptureDims(w, h);
    }

    // Bits per second for the export, from bits per pixel. The WebGPU game
    // needed a measured overshoot factor here because MediaRecorder treated
    // its bitrate as a suggestion and exceeded it by up to 1.94x; the MF
    // encoders land close enough to the request that the estimate below is
    // just the request, and there is nothing to calibrate.
    double exportRate(int w, int h) const {
        return std::clamp(double(w) * double(h) * fps() * double(bpp_), 4.0e6, 200.0e6);
    }

    void startExport() {
        if (exp_.active) return;
        const int64_t total = totalFrames();
        if (total <= 0) return;

        int w = 0, h = 0;
        exportDims(w, h);

        char name[64];
        std::snprintf(name, sizeof(name), "v7_edit_%03d.mp4", exportIndex_);

        VideoWriter::Config cfg;
        cfg.path = name;
        cfg.width = w;
        cfg.height = h;
        cfg.fpsNum = take_.fpsNum;
        cfg.fpsDen = take_.fpsDen;
        cfg.bitrate = uint32_t(exportRate(w, h));
        // Two seconds, against the recording's one. Nobody scrubs an export,
        // and a longer GOP is free quality at the same bitrate.
        cfg.gopFrames = int(std::lround(fps() * 2.0));

        if (!exp_.writer.open(cfg)) {
            status_ = "the encoder refused that size -- try a smaller one";
            return;
        }

        const size_t bytes = nv12Bytes(w, h);
        exp_.nv12 = device_->createBuffer(bytes,
                                          Falcor::ResourceBindFlags::UnorderedAccess |
                                              Falcor::ResourceBindFlags::ShaderResource,
                                          Falcor::MemoryType::DeviceLocal);
        exp_.read = device_->createBuffer(bytes, Falcor::ResourceBindFlags::None,
                                          Falcor::MemoryType::ReadBack);
        exp_.fence = device_->createFence();
        exp_.w = w;
        exp_.h = h;
        exp_.frame = 0;
        exp_.total = total;
        exp_.path = name;
        exp_.active = true;
        playing_ = false;
        status_.clear();
        ++exportIndex_;
    }

    void stepExport(Falcor::RenderContext *ctx) {
        // A fixed slice per rendered frame so the progress bar keeps moving
        // and the window keeps answering. Sixteen frames is ~25 ms of work at
        // 1080p, which is one slow frame of a UI that is showing a progress
        // bar and nothing else.
        for (int i = 0; i < kExportChunk && exp_.active; ++i) {
            if (exp_.frame >= exp_.total) {
                finishExport();
                return;
            }
            const int64_t src = sourceOf(exp_.frame);
            if (src < 0) {
                finishExport();
                return;
            }
            if (!decodeTo(ctx, src, srcTex_)) {
                // A frame the decoder will not give up. Hold the previous one
                // rather than abandoning the export or shortening the file:
                // the output stays constant rate and one frame is doubled.
                if (!exp_.writer.hasLastFrame()) {
                    status_ = "the source would not decode";
                    cancelExport();
                    return;
                }
                exp_.writer.repeatLast(exp_.frame, 1);
                ++exp_.frame;
                continue;
            }

            conv_->run(ctx, srcTex_, reader_.width(), reader_.height(), exp_.nv12, exp_.w, exp_.h,
                       /*srcIsSrgb*/ true);
            ctx->copyResource(exp_.read.get(), exp_.nv12.get());
            ctx->submit(false);
            const uint64_t v = ctx->signal(exp_.fence.get());
            // Synchronous, and only here. See the header: an export has no
            // clock to miss, so waiting is free in the only sense that
            // matters.
            exp_.fence->wait(v);
            const uint8_t *p = reinterpret_cast<const uint8_t *>(exp_.read->map());
            if (p) {
                exp_.writer.writeFrame(p, nv12Bytes(exp_.w, exp_.h), exp_.frame);
                exp_.read->unmap();
            }
            ++exp_.frame;
        }
    }

    void finishExport() {
        const bool ok = exp_.writer.close();
        char msg[256];
        std::snprintf(msg, sizeof(msg), "%s %s -- %lld frames at %dx%d",
                      ok ? "wrote" : "failed writing", exp_.path.c_str(),
                      (long long)exp_.frame, exp_.w, exp_.h);
        status_ = msg;
        std::printf("v7: %s\n", msg);
        std::fflush(stdout);
        exp_.reset();
        std::fflush(stdout);
    }

    void cancelExport() {
        if (!exp_.active) return;
        exp_.writer.close();
        if (!exp_.path.empty()) std::remove(exp_.path.c_str());
        exp_.reset();
        status_ = "export cancelled";
    }

    static void revealInExplorer(const std::string &path) {
        if (path.empty()) return;
        char full[MAX_PATH];
        if (!::GetFullPathNameA(path.c_str(), MAX_PATH, full, nullptr)) return;
        const std::string arg = std::string("/select,\"") + full + "\"";
        ::ShellExecuteA(nullptr, "open", "explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
    }

    static std::string timecode(double seconds) {
        if (seconds < 0.0) seconds = 0.0;
        const int total = int(seconds);
        char b[32];
        std::snprintf(b, sizeof(b), "%02d:%02d.%01d", total / 60, total % 60,
                      int((seconds - double(total)) * 10.0));
        return b;
    }

    static constexpr int kExportChunk = 16;

    enum class DragKind { None, Scrub, TrimIn, TrimOut };

    struct ExportState {
        bool active = false;
        VideoWriter writer;
        Falcor::ref<Falcor::Buffer> nv12, read;
        Falcor::ref<Falcor::Fence> fence;
        int w = 0, h = 0;
        int64_t frame = 0, total = 0;
        std::string path;
        void reset() {
            active = false;
            nv12 = nullptr;
            read = nullptr;
            fence = nullptr;
            frame = total = 0;
            path.clear();
        }
    };

    Falcor::ref<Falcor::Device> device_;
    Nv12Convert *conv_ = nullptr;

    VideoReader reader_;
    Take take_;
    std::vector<Clip> clips_;
    std::vector<uint8_t> rgba_;
    Falcor::ref<Falcor::Texture> preview_, srcTex_;

    bool open_ = false;
    bool playing_ = false;
    bool loop_ = true;
    int sel_ = 0;
    int64_t playhead_ = 0;
    int64_t shown_ = -1;
    int64_t dragPreview_ = -1;
    int64_t playFrom_ = 0;
    double playStart_ = 0.0;
    double now_ = 0.0;
    DragKind drag_ = DragKind::None;

    int sizeChoice_ = 0;
    float bpp_ = 0.10f;
    int exportIndex_ = 0;
    std::string status_;
    ExportState exp_;
};

}  // namespace vb
