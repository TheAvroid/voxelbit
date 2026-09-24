//! The window, laid out after Robinhood's asset page: a white page, the
//! asset's name and a big price that rolls digit by digit, the change in the
//! range's colour, a bare line chart you scrub with the pointer, range tabs
//! under it, a statistics grid, and an order-panel card on the right --
//! here the balance in bits and three buy pills. The layout is theirs; the
//! type is voxelbit's own 3x3-pixel.otf, lowercase throughout.

use std::sync::Arc;
use std::time::{Duration, Instant};

use chrono::{Local, TimeZone};
use eframe::egui::{
    self, Color32, CornerRadius, FontFamily, FontId, Margin, Painter, Pos2, Rect, Response,
    Sense, Shape, Stroke, UiBuilder, Vec2, WidgetInfo, WidgetType, pos2, vec2,
};

use crate::feed::{self, Link, Market, Range, Series, Shared, Wake};
use crate::units;

// Robinhood's own pair: a pure green for up, an orange-red for down, and
// everything else white or grey so those two carry the meaning.
//
// A WHITE PAGE WITH DARK GREY TEXT (user 2026-09-23, replacing the black
// page). TEXT is every primary word, the voxelbit mark included -- the
// website's gold is ~1.4:1 on white, i.e. invisible. MUTED stays a lighter
// grey for labels so the page keeps two levels.
const GREEN: Color32 = Color32::from_rgb(0, 200, 5);
const RED: Color32 = Color32::from_rgb(255, 80, 0);
const BG: Color32 = Color32::WHITE;
const TEXT: Color32 = Color32::from_rgb(51, 51, 51);
const MUTED: Color32 = Color32::from_rgb(134, 134, 139);
/// The chart past the scrub point, the dotted baseline, the scrub rule.
const DIM: Color32 = Color32::from_rgb(207, 207, 212);
const DIVIDER: Color32 = Color32::from_rgb(232, 232, 236);
const CARD: Color32 = Color32::WHITE;
const CARD_EDGE: Color32 = Color32::from_rgb(227, 227, 232);
/// The buy pills are GOLD (user 2026-09-23, replacing light green) -- the
/// website's own download button, stop for stop (.dl in website/index.html:
/// linear-gradient(180deg, #ffe9a6 0%, --gold 46%, #e0a72e 100%), #241a05
/// text, a 1 px white highlight inside the top edge).
const GOLD_RAMP: [(f32, Color32); 3] = [
    (0.0, Color32::from_rgb(0xff, 0xe9, 0xa6)),
    (0.46, Color32::from_rgb(0xff, 0xd7, 0x6a)),
    (1.0, Color32::from_rgb(0xe0, 0xa7, 0x2e)),
];
const GOLD_INK: Color32 = Color32::from_rgb(0x24, 0x1a, 0x05);
/// The balance card's outline and its text are gold too (user 2026-09-23),
/// then asked LIGHTER the same day: they were #e0a72e and #c08a12. The text
/// stops short of the site's --gold, which at ~1.4:1 on white is not
/// legible; this is ~2.1:1, readable at the card's two sizes.
const GOLD_EDGE: Color32 = Color32::from_rgb(0xf2, 0xd2, 0x84);
const GOLD_TEXT: Color32 = Color32::from_rgb(0xe3, 0xae, 0x3f);
/// The buy rows, in bits. The 1- and 10-bit rows were removed (user 2026-09-23).
const BUY_BITS: [u64; 3] = [100, 1_000, 10_000];

// ------------------------------------------------------------ the pixel face

use crate::PX3;

/// 3x3-pixel.otf is drawn on a 128-unit grid in a 640-unit em (see
/// engine/src/ui/app_hud.inl). egui scales by the em, so one pixel of the face
/// is size/5 screen pixels: ONLY PHYSICAL SIZES THAT ARE MULTIPLES OF FIVE
/// LAND ON WHOLE PIXELS, and anything else is a grey smear. Sizes below are
/// asked for in points and snapped here against the display's scaling.
fn px(ctx: &egui::Context, points: f32) -> f32 {
    let ppp = ctx.pixels_per_point();
    ((points * ppp / 5.0).round().max(1.0) * 5.0) / ppp
}

/// The face's line box is ascent 1024 + descent 256 = twice the size, and
/// its capitals (one em) start 0.6 of the size below the top. Text here is
/// placed by its CAP TOP, or every line would sit a size and a half apart.
const CAP_TOP: f32 = 0.6;
/// A line of text claims its capitals plus room for a descender.
const LINE: f32 = 1.4;

/// The pills HUG their text (user 2026-09-23): 34 tall for 15-pt capitals,
/// and PILL_PAD either side of the words -- they were 44 tall and stretched
/// to fill the card.
const BUTTON_H: f32 = 34.0;
/// How far a hovered button rises off the card: the website's .btn:hover
/// translateY(-3px), which the user set there ("doubled").
const LIFT: f32 = 3.0;
/// The balance card's corner radius and padding. 40 pt sits between the
/// iPhone X's screen corners (39) and the 12's (47); the padding grows with
/// it so nothing inside comes near the curve.
const CARD_R: f32 = 40.0;
const CARD_PAD: f32 = 26.0;
/// How long "coming with the wallet" stays under the pills after a press.
const PRESS_NOTE: Duration = Duration::from_secs(4);

const S_SMALL: f32 = 10.0;
const S_BODY: f32 = 15.0;
const S_TITLE: f32 = 20.0;
const S_PRICE: f32 = 40.0;
const S_BALANCE: f32 = 30.0;

fn font(size: f32) -> FontId {
    FontId::new(size, FontFamily::Proportional)
}

fn snap(p: Pos2, ppp: f32) -> Pos2 {
    pos2((p.x * ppp).round() / ppp, (p.y * ppp).round() / ppp)
}

// EVERYTHING ON SCREEN IS LOWERCASE (user 2026-09-23: "make everything
// lowercase"). It is enforced here, where every word the page draws passes,
// rather than trusted to each string: figures like "$1.69T" come out of
// units.rs capitalised, and a width measured on one case and painted in the
// other would misplace everything after it.
//
// THE EM DASH IS DRAWN, NOT TYPED (user 2026-09-23: "instead of = use —").
// The face has no U+2014 -- its cmap stops at the hyphen -- so a typed one
// would fall through to egui's smooth fallback font, the one curved glyph in
// a line of squares. website/index.html hit the same gap and draws its rules
// as boxes; this does the same: a bar at the face's own hyphen height and
// weight (one font pixel), 1.4 em long, a 1 em word gap either side. Spaces
// typed around the dash are dropped, since the gap already is one.
const DASH: char = '\u{2014}';
const DASH_GAP: f32 = 1.0;
const DASH_BAR: f32 = 1.4;

enum Run {
    Text(String),
    Dash,
}

fn runs(text: &str) -> Vec<Run> {
    let lower = text.to_lowercase();
    let parts: Vec<&str> = lower.split(DASH).collect();
    let mut out = Vec::new();
    for (i, part) in parts.iter().enumerate() {
        if i > 0 {
            out.push(Run::Dash);
        }
        let part = if i > 0 { part.trim_start() } else { part };
        let part = if i + 1 < parts.len() { part.trim_end() } else { part };
        if !part.is_empty() {
            out.push(Run::Text(part.to_string()));
        }
    }
    out
}

/// (top below the cap line, height), em -- read once from the font's hyphen.
fn hyphen() -> (f32, f32) {
    static H: std::sync::OnceLock<(f32, f32)> = std::sync::OnceLock::new();
    *H.get_or_init(crate::mark::hyphen_em)
}

fn text_width(painter: &Painter, text: &str, size: f32) -> f32 {
    runs(text)
        .into_iter()
        .map(|r| match r {
            Run::Text(t) => painter.layout_no_wrap(t, font(size), TEXT).size().x,
            Run::Dash => (2.0 * DASH_GAP + DASH_BAR) * size,
        })
        .sum()
}

/// Paint `text` with its cap top-left at `at`; returns its width.
fn paint_text(painter: &Painter, at: Pos2, size: f32, text: &str, color: Color32) -> f32 {
    let ppp = painter.ctx().pixels_per_point();
    let mut x = at.x;
    for run in runs(text) {
        match run {
            Run::Text(t) => {
                let galley = painter.layout_no_wrap(t, font(size), color);
                let w = galley.size().x;
                painter.galley(snap(pos2(x, at.y - CAP_TOP * size), ppp), galley, color);
                x += w;
            }
            Run::Dash => {
                let (top, h) = hyphen();
                let a = snap(pos2(x + DASH_GAP * size, at.y + top * size), ppp);
                let b = snap(pos2(x + (DASH_GAP + DASH_BAR) * size, at.y + (top + h) * size), ppp);
                painter.rect_filled(Rect::from_min_max(a, b), 0.0, color);
                x += (2.0 * DASH_GAP + DASH_BAR) * size;
            }
        }
    }
    x - at.x
}

/// A label that takes only the room its letters use.
fn label(ui: &mut egui::Ui, text: &str, size: f32, color: Color32) -> Response {
    let w = text_width(ui.painter(), text, size);
    let (rect, resp) = ui.allocate_exact_size(vec2(w, size * LINE), Sense::hover());
    paint_text(ui.painter(), rect.left_top(), size, text, color);
    resp
}

// ------------------------------------------------------------ rolling price

/// The price the way Robinhood shows it: a digit that changes rolls out and
/// its replacement rolls in -- upward on an up-tick, downward on a down-tick
/// -- while the digits that did not change stay still. It only works because
/// every digit in this face is the same six cells wide, so a changed price
/// of the same length puts every character exactly where it was.
struct Roller {
    text: String,
    prev: String,
    since: Instant,
    dir: i8,
}

const ROLL: Duration = Duration::from_millis(320);

impl Roller {
    fn new() -> Self {
        Self { text: String::new(), prev: String::new(), since: Instant::now(), dir: 1 }
    }

    /// `animate` is false while scrubbing the chart: those numbers are a
    /// readout, not a tick, and rolling every one of them would smear.
    fn set(&mut self, text: String, dir: i8, animate: bool) {
        if text == self.text {
            return;
        }
        self.prev = if animate { std::mem::take(&mut self.text) } else { text.clone() };
        self.text = text;
        self.since = Instant::now();
        self.dir = dir;
    }

    fn rolling(&self) -> bool {
        self.since.elapsed() < ROLL
    }

    fn show(&self, ui: &mut egui::Ui, size: f32, color: Color32) {
        let chars: Vec<char> = self.text.chars().collect();
        let prev: Vec<char> = self.prev.chars().collect();
        let same_len = prev.len() == chars.len();
        let t = (self.since.elapsed().as_secs_f32() / ROLL.as_secs_f32()).min(1.0);
        let ease = 1.0 - (1.0 - t).powi(3);

        let painter = ui.painter().clone();
        let widths: Vec<f32> = chars.iter().map(|c| text_width(&painter, &c.to_string(), size)).collect();
        let (rect, _) = ui.allocate_exact_size(vec2(widths.iter().sum(), size * 1.15), Sense::hover());
        let mut x = rect.left();
        for (i, c) in chars.iter().enumerate() {
            let at = pos2(x, rect.top());
            if same_len && t < 1.0 && prev[i] != *c {
                let travel = size * 1.3;
                let d = if self.dir >= 0 { 1.0 } else { -1.0 };
                let cell = Rect::from_min_size(pos2(x, rect.top() - size * 0.1), vec2(widths[i], size * 1.2));
                let clipped = painter.with_clip_rect(cell);
                paint_text(&clipped, at - vec2(0.0, d * travel * ease), size, &prev[i].to_string(), color);
                paint_text(&clipped, at + vec2(0.0, d * travel * (1.0 - ease)), size, &c.to_string(), color);
            } else {
                paint_text(&painter, at, size, &c.to_string(), color);
            }
            x += widths[i];
        }
    }
}

// ------------------------------------------------------------ the app

pub struct WalletApp {
    market: Shared,
    wake: Wake,
    /// false in the render test, which fills the market itself.
    fetch: bool,
    range: Range,
    /// The wallet's balance, in sats like every amount here. There is no
    /// wallet behind it yet, so it starts -- and stays -- at 0.
    balance_sats: u64,
    /// When buy or sell was last pressed, for the note under them.
    pressed: Option<Instant>,
    roller: Roller,
    /// The chart point under the pointer, from the previous frame. The
    /// header is drawn before the chart knows; one frame of lag is invisible
    /// and saves laying the page out twice.
    hover: Option<(i64, f64)>,
    /// The game's crosshair replaces the system cursor -- but only once its
    /// GPU pass exists, or the window would have no pointer at all.
    crosshair: bool,
    /// The website's wordmark, composited once per display scale.
    mark: Option<MarkTex>,
    /// The buy pills' gradient fill, made once.
    ramp: Option<egui::TextureHandle>,
}

struct MarkTex {
    em: f32,
    tex: egui::TextureHandle,
    origin: [f32; 2],
    baseline: f32,
    width: f32,
}

impl WalletApp {
    pub fn new(cc: &eframe::CreationContext<'_>) -> Self {
        style(&cc.egui_ctx);
        let market: Shared = Default::default();
        let ctx = cc.egui_ctx.clone();
        let wake: Wake = Arc::new(move || ctx.request_repaint());
        feed::spawn(market.clone(), wake.clone());
        let mut app = Self::with_market(market, wake, true);
        app.crosshair = crate::cross::install(cc.wgpu_render_state.as_ref());
        app
    }

    pub fn with_market(market: Shared, wake: Wake, fetch: bool) -> Self {
        Self {
            market,
            wake,
            fetch,
            range: Range::Day,
            balance_sats: 0,
            pressed: None,
            roller: Roller::new(),
            hover: None,
            crosshair: false,
            mark: None,
            ramp: None,
        }
    }

    pub fn draw(&mut self, ui: &mut egui::Ui) {
        let market = self.market.clone();
        let m = market.lock().unwrap();

        let full = ui.max_rect();
        ui.painter().rect_filled(full, 0.0, BG);
        ui.spacing_mut().item_spacing = Vec2::ZERO;

        let bar = Rect::from_min_size(full.min, vec2(full.width(), 64.0));
        top_bar(ui, bar, &m, &mut self.mark);
        let body = Rect::from_min_max(pos2(full.left(), bar.bottom() + 1.0), full.max);
        ui.scope_builder(UiBuilder::new().max_rect(body), |ui| {
            egui::ScrollArea::vertical().auto_shrink([false, false]).show(ui, |ui| self.page(ui, &m));
        });

        let hovering = self.hover.is_some();
        let rolling = self.roller.rolling();
        drop(m);

        // After the lock is released: request_history takes it too. The year
        // is always wanted, for the 52-week figures.
        if self.fetch {
            feed::request_history(&self.market, self.range, &self.wake);
            feed::request_history(&self.market, Range::Year, &self.wake);
        }
        let ctx = ui.ctx();
        if self.crosshair {
            crate::cross::show(ctx);
        }
        if rolling {
            ctx.request_repaint();
        } else if !hovering {
            // The live dot's pulse. 30 fps is plenty for a ring that grows.
            ctx.request_repaint_after(Duration::from_millis(33));
        }
    }

    fn page(&mut self, ui: &mut egui::Ui, m: &Market) {
        ui.spacing_mut().item_spacing = Vec2::ZERO;
        let avail = ui.available_width();
        let content = (avail - 48.0).min(1120.0);
        // The card is as wide as its widest buy row needs, measured at the
        // live price: a fixed width was outgrown twice in one afternoon
        // ("buy 100 bits" overlapped its price at 340; "buy 10,000 bits" and
        // "$840.51" need 407 px inside), and the prices grow with bitcoin.
        let (side, gap) = (card_width(ui, m).max(300.0), 48.0);
        let wide = content - side - gap >= 500.0;
        let left = if wide { content - side - gap } else { content };
        let lead = ((avail - content) / 2.0).max(24.0);

        ui.add_space(24.0);
        ui.horizontal_top(|ui| {
            ui.add_space(lead);
            ui.vertical(|ui| {
                ui.set_width(left);
                self.main_column(ui, m, left);
                if !wide {
                    ui.add_space(40.0);
                    self.balance_card(ui, m, left);
                }
            });
            if wide {
                ui.add_space(gap);
                ui.vertical(|ui| {
                    ui.set_width(side);
                    self.balance_card(ui, m, side);
                });
            }
        });
        ui.add_space(48.0);
    }

    fn main_column(&mut self, ui: &mut egui::Ui, m: &Market, w: f32) {
        let ctx = ui.ctx().clone();
        let series = m.series.get(&self.range).filter(|s| s.points.len() >= 2);
        let first = series.map(|s| s.points[0].1);
        let latest = m.price.or(series.map(|s| s.points[s.points.len() - 1].1));
        // The range's colour: up or down over the whole range, at the live
        // price. Scrubbing does not repaint the chart in another colour.
        let theme = match (latest, first) {
            (Some(p), Some(f)) if p < f => RED,
            _ => GREEN,
        };

        label(ui, "bitcoin", px(&ctx, S_TITLE), TEXT);
        ui.add_space(10.0);

        let shown = self.hover.map(|h| h.1).or(m.price);
        let text = shown.map_or("$--".to_string(), |p| units::fmt_usd(p, 2));
        let dir = m.tick.map_or(1, |t| t.0);
        self.roller.set(text, dir, self.hover.is_none());
        self.roller.show(ui, px(&ctx, S_PRICE), TEXT);
        ui.add_space(14.0);

        if let (Some(p), Some(f)) = (shown, first) {
            change_line(ui, p - f, (p / f - 1.0) * 100.0, self.range.words(), px(&ctx, S_BODY));
        } else {
            label(ui, " ", px(&ctx, S_BODY), MUTED);
        }
        ui.add_space(10.0);

        // The bits, directly under the total -- what this wallet counts in.
        if let Some(p) = m.price {
            let s = px(&ctx, S_SMALL);
            ui.horizontal(|ui| {
                let per_bit = units::fmt_usd(units::sats_to_usd(units::SATS_PER_BIT, p), 2);
                label(ui, &format!("1 bit \u{2014} {per_bit}"), s, MUTED);
                ui.add_space(28.0);
                label(ui, &format!("{} bits \u{2014} 1 btc", units::group(units::BITS_PER_BTC)), s, MUTED);
            });
        }
        ui.add_space(28.0);

        self.chart(ui, m, series, theme, w);
        ui.add_space(12.0);
        self.tabs(ui, theme);
        ui.add_space(32.0);
        stats(ui, m);
    }

    fn chart(&mut self, ui: &mut egui::Ui, m: &Market, series: Option<&Series>, theme: Color32, w: f32) {
        let ctx = ui.ctx().clone();
        let ppp = ctx.pixels_per_point();
        let (rect, resp) = ui.allocate_exact_size(vec2(w, 280.0), Sense::hover());
        let painter = ui.painter().clone();

        let Some(s) = series else {
            self.hover = None;
            let small = px(&ctx, S_SMALL);
            let msg = match (&m.history_error, m.loading.contains(&self.range)) {
                (Some(e), false) => format!("couldn't load the chart -- retrying. {e}"),
                _ => format!("loading {}...", self.range.words()),
            };
            let tw = text_width(&painter, &msg, small).min(w);
            paint_text(&painter, pos2(rect.center().x - tw / 2.0, rect.center().y), small, &msg, MUTED);
            return;
        };

        // The top strip holds the scrub timestamp; the right margin is room
        // for the live dot's ring.
        let label_h = 26.0;
        let plot = Rect::from_min_max(pos2(rect.left(), rect.top() + label_h), pos2(rect.right() - 10.0, rect.bottom() - 6.0));
        let (t0, t1) = (s.points[0].0, s.points[s.points.len() - 1].0);
        let span = (t1 - t0).max(1) as f32;
        let (hi, lo) = s.high_low().unwrap();
        let pad = ((hi - lo) * 0.06).max(hi * 0.0002);
        let (lo, hi) = (lo - pad, hi + pad);
        let at = |t: i64, p: f64| {
            pos2(
                plot.left() + (t - t0) as f32 / span * plot.width(),
                plot.bottom() - ((p - lo) / (hi - lo)) as f32 * plot.height(),
            )
        };
        let pts: Vec<Pos2> = s.points.iter().map(|&(t, p)| at(t, p)).collect();
        let stroke = Stroke::new(2.0, theme);

        // Robinhood's dotted line at where the day opened; here, where the
        // range opened, on the two short ranges where it is a reference and
        // not just the chart's left edge.
        if matches!(self.range, Range::Live | Range::Day) {
            let y = (pts[0].y * ppp).round() / ppp + 0.5 / ppp;
            let mut x = plot.left();
            while x < plot.right() {
                painter.circle_filled(pos2(x, y), 1.0, DIM);
                x += 7.0;
            }
        }

        let pointer = resp.hover_pos().filter(|p| rect.contains(*p));
        match pointer {
            Some(p) => {
                let i = nearest(&pts, p.x);
                // Scrubbed past is the range's colour; not yet reached is grey,
                // and the fill under each half follows its line.
                fill_under(&painter, &pts[..=i], plot.top(), rect.bottom(), theme);
                fill_under(&painter, &pts[i..], plot.top(), rect.bottom(), DIM);
                painter.add(Shape::line(pts[..=i].to_vec(), stroke));
                painter.add(Shape::line(pts[i..].to_vec(), Stroke::new(2.0, DIM)));
                let x = ((pts[i].x * ppp).floor() + 0.5) / ppp;
                painter.line_segment([pos2(x, rect.top() + label_h - 4.0), pos2(x, rect.bottom())], Stroke::new(1.0, DIM));
                painter.circle_filled(pts[i], 5.0, theme);
                painter.circle_stroke(pts[i], 5.0, Stroke::new(2.0, BG));

                let small = px(&ctx, S_SMALL);
                let when = fmt_moment(s.points[i].0, self.range);
                let tw = text_width(&painter, &when, small);
                let lx = (x - tw / 2.0).clamp(rect.left(), rect.right() - tw);
                paint_text(&painter, pos2(lx, rect.top() + 4.0), small, &when, MUTED);
                self.hover = Some(s.points[i]);
            }
            None => {
                fill_under(&painter, &pts, plot.top(), rect.bottom(), theme);
                painter.add(Shape::line(pts.clone(), stroke));
                let end = *pts.last().unwrap();
                // The live dot, and a ring that keeps leaving it.
                let phase = (ctx.input(|i| i.time) % 1.6 / 1.6) as f32;
                painter.circle_filled(end, 4.0 + 12.0 * phase, theme.gamma_multiply(0.45 * (1.0 - phase)));
                painter.circle_filled(end, 4.5, theme);
                self.hover = None;
            }
        }
    }

    fn tabs(&mut self, ui: &mut egui::Ui, theme: Color32) {
        let ctx = ui.ctx().clone();
        let size = px(&ctx, S_SMALL);
        let mut underline = None;
        let row = ui.horizontal(|ui| {
            for r in Range::ALL {
                let on = r == self.range;
                let tw = text_width(ui.painter(), r.label(), size);
                let (rect, resp) = ui.allocate_exact_size(vec2(tw + 28.0, 40.0), Sense::click());
                resp.widget_info(|| WidgetInfo::selected(WidgetType::Button, true, on, r.label()));
                let color = if on || resp.hovered() { theme } else { TEXT };
                let x = rect.center().x - tw / 2.0;
                paint_text(ui.painter(), pos2(x, rect.center().y - size / 2.0), size, r.label(), color);
                if on {
                    underline = Some((x - 4.0, x + tw + 4.0));
                }
                if resp.clicked() && !on {
                    self.range = r;
                    self.hover = None;
                }
            }
        });
        // One rule across the whole column, the chosen tab underlined on it.
        let y = row.response.rect.bottom();
        let w = ui.available_width();
        let left = row.response.rect.left();
        ui.painter().rect_filled(Rect::from_min_size(pos2(left, y), vec2(w, 1.0)), 0.0, DIVIDER);
        if let Some((a, b)) = underline {
            ui.painter().rect_filled(Rect::from_min_max(pos2(a, y - 1.0), pos2(b, y + 1.0)), 0.0, theme);
        }
    }

    /// The right-hand card: "balance", the balance IN BITS (user 2026-09-23:
    /// "the balance doesnt show usd but rather how many bits you have"), and
    /// three buy pills -- 1, 10 and 100 bits, each with what it costs in
    /// dollars at the live price. (Buy/sell under the balance came and went
    /// the same day; the pills replace them.) Nothing trades yet, so a press
    /// says so for a few seconds rather than doing nothing, which would read
    /// as a broken button.
    fn balance_card(&mut self, ui: &mut egui::Ui, m: &Market, w: f32) {
        let ctx = ui.ctx().clone();
        let (small, body) = (px(&ctx, S_SMALL), px(&ctx, S_BODY));
        let balance = format!("{} bits", units::fmt_bits(self.balance_sats));
        let ramp = self.ramp.get_or_insert_with(|| ramp_texture(&ctx, "gold-ramp", &GOLD_RAMP)).clone();

        // The card is drawn BEHIND its contents once their size is known:
        // two placeholders now, filled in after the frame has laid out.
        let shadow_slot = ui.painter().add(Shape::Noop);
        let card_slot = ui.painter().add(Shape::Noop);
        let card = egui::Frame::new().inner_margin(Margin::same(CARD_PAD as i8)).show(ui, |ui| {
            ui.spacing_mut().item_spacing = Vec2::ZERO;
            ui.set_width(w - 2.0 * CARD_PAD);
            label(ui, "balance", body, GOLD_TEXT);
            ui.add_space(12.0);
            label(ui, &balance, px(&ctx, S_BALANCE), GOLD_TEXT);
            ui.add_space(24.0);

            // TWO PILLS A ROW (user 2026-09-23: "seperate the buy 1 bit and
            // the price. they should each have their own pill next to each
            // other"): the amount on the left, what it costs beside it. Each
            // pill is as wide as its own words, so the rows are ragged on the
            // right, the way tags are.
            for (i, bits) in BUY_BITS.into_iter().enumerate() {
                if i > 0 {
                    ui.add_space(10.0);
                }
                let (amount, price) = (buy_label(bits), buy_price(bits, m));
                let (aw, pw) = (pill_width(ui.painter(), &amount, body), pill_width(ui.painter(), &price, body));
                // THE ROW IS ONE BUTTON: pressing either pill buys, and
                // hovering either lifts both, so the pair reads as one thing.
                // Its hit area is the two pills, not the width of the card.
                let (slot, resp) = ui.allocate_exact_size(vec2(aw + PILL_GAP + pw, BUTTON_H), Sense::click());
                // The word "buy" is off the pill (user 2026-09-23), but it is
                // still what the button does, so a screen reader says it.
                resp.widget_info(|| WidgetInfo::labeled(WidgetType::Button, true, format!("buy {amount}")));
                let pressed = resp.is_pointer_button_down_on();
                // HOVER LIFTS IT (user 2026-09-23: "make them hover"): up off
                // the card, a deeper shadow under it; a press sets it back
                // down. The hit area stays put, so the pills cannot slide out
                // from under the pointer that is lifting them.
                let lift = ctx.animate_bool_with_time(resp.id.with("lift"), resp.hovered() && !pressed, 0.16);
                let row = slot.translate(vec2(0.0, -LIFT * lift));
                let amount_rect = Rect::from_min_size(row.min, vec2(aw, BUTTON_H));
                let price_rect = Rect::from_min_size(pos2(row.left() + aw + PILL_GAP, row.top()), vec2(pw, BUTTON_H));
                let painter = ui.painter();
                for (rect, text) in [(amount_rect, amount.as_str()), (price_rect, price.as_str())] {
                    gold_pill(painter, rect, &ramp, lift, pressed);
                    let tw = text_width(painter, text, body);
                    paint_text(painter, pos2(rect.center().x - tw / 2.0, rect.center().y - body / 2.0), body, text, GOLD_INK);
                }
                if resp.clicked() {
                    self.pressed = Some(Instant::now());
                }
            }

            if self.pressed.is_some_and(|t| t.elapsed() < PRESS_NOTE) {
                ui.add_space(14.0);
                ui.vertical_centered(|ui| {
                    label(ui, "coming with the wallet", small, MUTED);
                });
                ctx.request_repaint_after(Duration::from_millis(250));
            }
        });

        let rect = card.response.rect;
        // White on white needs an edge AND a lift, as Robinhood's light panel
        // has. The shadow is blurred past telling circle from squircle, so
        // egui's own rounded shadow stands in for it.
        let shadow = egui::Shadow { offset: [0, 2], blur: 14, spread: 0, color: Color32::from_black_alpha(16) };
        ui.painter().set(shadow_slot, shadow.as_shape(rect, CornerRadius::same(CARD_R as u8)));
        ui.painter().set(card_slot, Shape::convex_polygon(squircle(rect, CARD_R), CARD, Stroke::new(1.5, GOLD_EDGE)));
    }
}

impl eframe::App for WalletApp {
    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        self.draw(ui);
    }
}

pub fn style(ctx: &egui::Context) {
    let mut fonts = egui::FontDefinitions::default();
    // Pixel outlines on a whole-pixel grid want neither hinting (it nudges
    // edges it has no reason to) nor sub-pixel placement (which blurs them).
    let tweak = egui::FontTweak { hinting: Some(false), subpixel_binning: Some(false), ..Default::default() };
    fonts.font_data.insert("px3".into(), Arc::new(egui::FontData::from_static(PX3).tweak(tweak)));
    // First in both families; egui's own faces stay behind it for the few
    // characters the pixel face does not have.
    for family in [FontFamily::Proportional, FontFamily::Monospace] {
        fonts.families.entry(family).or_default().insert(0, "px3".into());
    }
    ctx.set_fonts(fonts);

    ctx.set_theme(egui::Theme::Light);
    ctx.style_mut_of(egui::Theme::Light, |s| {
        s.visuals.panel_fill = BG;
        s.visuals.window_fill = BG;
        s.visuals.extreme_bg_color = BG;
        s.visuals.selection.bg_fill = GREEN.gamma_multiply(0.25);
        s.visuals.text_cursor.stroke.color = GREEN;
        s.visuals.widgets.inactive.bg_stroke = Stroke::new(1.0, CARD_EDGE);
        s.visuals.widgets.hovered.bg_stroke = Stroke::new(1.0, MUTED);
        s.visuals.widgets.active.bg_stroke = Stroke::new(1.0, GREEN);
        s.visuals.selection.stroke = Stroke::new(1.0, GREEN);
    });
}

/// The wordmark's em, in points. Snapped below to a multiple of 15 physical
/// pixels: the bevel step and the letter-spacing are em/15 and the font's
/// pixel em/5, so all three land on whole pixels.
const S_MARK: f32 = 30.0;

fn top_bar(ui: &mut egui::Ui, bar: Rect, m: &Market, mark: &mut Option<MarkTex>) {
    let ctx = ui.ctx().clone();
    let painter = ui.painter();
    let small = px(&ctx, S_SMALL);
    let ppp = ctx.pixels_per_point();

    let em = (S_MARK * ppp / 15.0).round().max(1.0) * 15.0;
    if mark.as_ref().is_none_or(|mk| mk.em != em) {
        let mk = crate::mark::render(em);
        let tex = ctx.load_texture("voxelbit-mark", mk.image, egui::TextureOptions::NEAREST);
        *mark = Some(MarkTex { em, tex, origin: mk.origin, baseline: mk.baseline, width: mk.width });
    }
    let mk = mark.as_ref().unwrap();
    // In physical pixels, so the image lands 1:1. The capitals span 0.6 to
    // 1.6 em of the line box; centre them in the bar.
    let box_left = ((bar.left() + 24.0) * ppp).round();
    let box_top = (bar.center().y * ppp - 1.1 * em).round();
    let min = pos2((box_left - mk.origin[0]) / ppp, (box_top - mk.origin[1]) / ppp);
    let uv = Rect::from_min_max(pos2(0.0, 0.0), pos2(1.0, 1.0));
    painter.image(mk.tex.id(), Rect::from_min_size(min, mk.tex.size_vec2() / ppp), uv, Color32::WHITE);
    // "wallet" sits on the mark's baseline.
    let baseline = (box_top + mk.baseline) / ppp;
    paint_text(painter, pos2((box_left + mk.width) / ppp + 10.0, baseline - small), small, "wallet", MUTED);

    let (color, text) = match &m.link {
        Link::Live if m.last_heard.is_some_and(|t| t.elapsed() > Duration::from_secs(3)) => (RED, "stale"),
        Link::Live => (GREEN, "live"),
        Link::Connecting => (MUTED, "connecting"),
        Link::Down(_) => (RED, "reconnecting"),
    };
    let tw = text_width(painter, text, small);
    let x = bar.right() - 24.0 - tw;
    paint_text(painter, pos2(x, bar.center().y - small / 2.0), small, text, color);
    painter.circle_filled(pos2(x - 10.0, bar.center().y), 3.5, color);
    painter.rect_filled(Rect::from_min_size(pos2(bar.left(), bar.bottom()), vec2(bar.width(), 1.0)), 0.0, DIVIDER);
    if let Link::Down(why) = &m.link {
        let r = Rect::from_min_max(pos2(x - 16.0, bar.top()), bar.max);
        ui.interact(r, ui.id().with("link"), Sense::hover()).on_hover_text(why.to_lowercase());
    }
}

/// "▲ $412.30 (0.49%)  past day" -- the triangle is drawn, because the pixel
/// face has no arrows and a fallback glyph would be the one smooth shape in
/// a line of squares.
fn change_line(ui: &mut egui::Ui, delta: f64, pct: f64, words: &str, size: f32) {
    let up = delta >= 0.0;
    let color = if up { GREEN } else { RED };
    ui.horizontal(|ui| {
        let (rect, _) = ui.allocate_exact_size(vec2(size * 0.8, size * LINE), Sense::hover());
        let (l, r, top, bot) = (rect.left(), rect.left() + size * 0.8, rect.top() + size * 0.1, rect.top() + size * 0.9);
        let tri = if up {
            vec![pos2(l, bot), pos2(r, bot), pos2((l + r) / 2.0, top)]
        } else {
            vec![pos2(l, top), pos2(r, top), pos2((l + r) / 2.0, bot)]
        };
        ui.painter().add(Shape::convex_polygon(tri, color, Stroke::NONE));
        ui.add_space(size * 0.5);
        let text = format!("{} ({:.2}%)", units::fmt_usd(delta.abs(), 2), pct.abs());
        label(ui, &text, size, color);
        ui.add_space(size);
        label(ui, words, size, MUTED);
    });
}

fn stats(ui: &mut egui::Ui, m: &Market) {
    let ctx = ui.ctx().clone();
    let (small, body) = (px(&ctx, S_SMALL), px(&ctx, S_BODY));
    let w = ui.available_width();
    // No heading (user 2026-09-23: "remove the key statistics text"): the
    // grid sits straight under the range tabs' rule. And three figures, not
    // eight -- coinbase volume, supply, block height and the 24h high/low were
    // taken out the same day. The chain tip is still fetched: the market cap
    // is computed from the supply at that height.

    let usd = |v: Option<f64>| v.map_or("--".to_string(), |v| units::fmt_usd(v, 0));
    let supply = m.height.map(units::supply_at_height);
    let year = m.series.get(&Range::Year).and_then(Series::high_low);
    let cells = [
        ("market cap", match (supply, m.price) {
            (Some(s), Some(p)) => units::fmt_usd_compact(units::sats_to_usd(s, p)),
            _ => "--".into(),
        }),
        ("52 week high", usd(year.map(|y| y.0))),
        ("52 week low", usd(year.map(|y| y.1))),
    ];
    let cols = 3;
    let col_w = w / cols as f32;
    for row in cells.chunks(cols) {
        ui.horizontal(|ui| {
            for (name, value) in row {
                let (rect, _) = ui.allocate_exact_size(vec2(col_w, small * LINE + 10.0 + body * LINE), Sense::hover());
                paint_text(ui.painter(), rect.left_top(), small, name, MUTED);
                paint_text(ui.painter(), rect.left_top() + vec2(0.0, small * LINE + 10.0), body, value, TEXT);
            }
        });
        ui.add_space(26.0);
    }
}

/// The fill under the line (user 2026-09-23, from a screenshot of a finance
/// chart): the line's own colour fading to nothing at the chart's floor.
///
/// The alpha is a function of HEIGHT in the plot, not of distance below the
/// line, so one vertical ramp runs through every column and a peak reads
/// stronger than a trough -- as in the screenshot. A quad from each segment
/// down to the floor, with that alpha at its four corners, reproduces the ramp
/// exactly: the GPU interpolates colour linearly and the ramp is linear in y.
fn fill_under(painter: &Painter, pts: &[Pos2], top: f32, floor: f32, color: Color32) {
    const PEAK: f32 = 0.22;
    if pts.len() < 2 {
        return;
    }
    let tint = |y: f32| {
        let a = PEAK * ((floor - y) / (floor - top)).clamp(0.0, 1.0);
        Color32::from_rgba_unmultiplied(color.r(), color.g(), color.b(), (a * 255.0).round() as u8)
    };
    let mut mesh = egui::Mesh::default();
    for p in pts {
        mesh.colored_vertex(*p, tint(p.y));
        mesh.colored_vertex(pos2(p.x, floor), Color32::TRANSPARENT);
    }
    for i in 0..(pts.len() - 1) as u32 {
        let (a, b, c, d) = (2 * i, 2 * i + 1, 2 * i + 2, 2 * i + 3);
        mesh.add_triangle(a, b, c);
        mesh.add_triangle(c, b, d);
    }
    painter.add(Shape::mesh(mesh));
}

/// The outline of a rect with iPhone corners (user 2026-09-23: "make the
/// corners of the wallet box like the corner radius of the iphone").
///
/// Apple's corners are CONTINUOUS, not circular: a circle's curvature jumps
/// from zero to 1/r where the edge meets the arc, and that jump is the seam
/// the eye catches. Apple's curve starts about 1.528 r along each edge and
/// eases its curvature in from zero. Each corner here is a superellipse
/// quarter over that 1.528 r, with its exponent fixed so that at 45 degrees
/// it passes exactly where a circular corner of radius r would -- so it reads
/// as the same size of corner, only without the seam:
///     1.528 r (1 - 2^(-1/n)) = r (1 - 1/sqrt 2)   =>   n = 3.26
fn squircle(rect: Rect, r: f32) -> Vec<Pos2> {
    const EXTENT: f32 = 1.528;
    const N: f32 = 3.26;
    const STEPS: usize = 24;
    let e = (r * EXTENT).min(rect.width() / 2.0).min(rect.height() / 2.0);
    // A power below 1 magnifies float noise: sin(pi) comes back as -8.7e-8,
    // which 2/N turns into a 3-thousandths-of-a-pixel slip off the edge. The
    // trig is exact at the quarter turns, so snap what is meant to be zero.
    let pow = |v: f32| if v.abs() < 1e-6 { 0.0 } else { v.signum() * v.abs().powf(2.0 / N) };
    // Clockwise on screen (y down): each corner's quarter, from the edge
    // it arrives on to the edge it leaves by.
    let corners = [
        (pos2(rect.left() + e, rect.top() + e), std::f32::consts::PI),
        (pos2(rect.right() - e, rect.top() + e), 1.5 * std::f32::consts::PI),
        (pos2(rect.right() - e, rect.bottom() - e), 0.0),
        (pos2(rect.left() + e, rect.bottom() - e), 0.5 * std::f32::consts::PI),
    ];
    let mut pts = Vec::with_capacity(4 * (STEPS + 1));
    for (c, start) in corners {
        for k in 0..=STEPS {
            let t = start + std::f32::consts::FRAC_PI_2 * k as f32 / STEPS as f32;
            pts.push(pos2(c.x + e * pow(t.cos()), c.y + e * pow(t.sin())));
        }
    }
    pts
}

/// A button's fill (user 2026-09-23: "lighter at the top and darker at the
/// bottom"), a vertical CSS-style gradient from `stops` baked into a 1 x 64
/// texture the rounded rect is filled from. A texture rather than a
/// hand-built mesh because the rect keeps egui's anti-aliased round ends that
/// way; a raw mesh would be jagged at every curve.
fn ramp_texture(ctx: &egui::Context, name: &str, stops: &[(f32, Color32)]) -> egui::TextureHandle {
    const TEXELS: usize = 64;
    let mix = |a: u8, b: u8, t: f32| (a as f32 + (b as f32 - a as f32) * t).round() as u8;
    let at = |t: f32| {
        let i = stops.windows(2).position(|w| t <= w[1].0).unwrap_or(stops.len() - 2);
        let ((t0, a), (t1, b)) = (stops[i], stops[i + 1]);
        let k = ((t - t0) / (t1 - t0)).clamp(0.0, 1.0);
        Color32::from_rgb(mix(a.r(), b.r(), k), mix(a.g(), b.g(), k), mix(a.b(), b.b(), k))
    };
    let pixels = (0..TEXELS).map(|i| at(i as f32 / (TEXELS - 1) as f32)).collect();
    ctx.load_texture(name, egui::ColorImage::new([1, TEXELS], pixels), egui::TextureOptions::LINEAR)
}

/// What a buy pill says: the amount, without "buy" (user 2026-09-23).
fn buy_label(bits: u64) -> String {
    if bits == 1 { "1 bit".into() } else { format!("{} bits", units::group(bits)) }
}

fn buy_price(bits: u64, m: &Market) -> String {
    match m.price {
        Some(p) => units::fmt_usd(units::sats_to_usd(bits * units::SATS_PER_BIT, p), 2),
        None => "$--".into(),
    }
}

/// Room either side of a pill's words, in from its round ends.
const PILL_PAD: f32 = 14.0;
/// Between a row's amount pill and its price pill.
const PILL_GAP: f32 = 8.0;

fn pill_width(painter: &Painter, text: &str, size: f32) -> f32 {
    text_width(painter, text, size) + 2.0 * PILL_PAD
}

/// The balance card's width: whatever is widest inside it -- a buy row at
/// the live price, or the balance itself -- plus the card's padding. The
/// page asks before laying out, so the column beside it gets the rest.
fn card_width(ui: &egui::Ui, m: &Market) -> f32 {
    let ctx = ui.ctx();
    let (body, big) = (px(ctx, S_BODY), px(ctx, S_BALANCE));
    let painter = ui.painter();
    let rows = BUY_BITS
        .iter()
        .map(|&b| pill_width(painter, &buy_label(b), body) + PILL_GAP + pill_width(painter, &buy_price(b, m), body))
        .fold(0.0, f32::max);
    // The balance never shrinks the card below today's zero, so a first buy
    // does not jolt the layout.
    let balance = text_width(painter, "0 bits", big);
    rows.max(balance) + 2.0 * CARD_PAD
}

/// One gold pill, as the website's download button draws it: the ramp, a
/// 1 px white highlight inside the top edge (its `inset 0 1px 0
/// rgba(255,255,255,.55)`), and a shadow. The site's shadow is 45% black
/// because it sits on dark video; on this white page that reads as a smudge,
/// so it is lighter here and deepens as the pill lifts.
fn gold_pill(painter: &Painter, rect: Rect, ramp: &egui::TextureHandle, lift: f32, pressed: bool) {
    let round = CornerRadius::same((rect.height() / 2.0) as u8);
    let a = (18.0 + 26.0 * lift).round() as u8;
    let shadow = egui::Shadow { offset: [0, (3.0 + 3.0 * lift) as i8], blur: (10.0 + 6.0 * lift) as u8, spread: 0, color: Color32::from_black_alpha(a) };
    painter.add(shadow.as_shape(rect, round));
    // The ramp is multiplied by the fill: white shows it as made, a press
    // dims the whole of it.
    let tint = if pressed { shade(Color32::WHITE, 0.9) } else { Color32::WHITE };
    let uv = Rect::from_min_max(pos2(0.0, 0.0), pos2(1.0, 1.0));
    painter.add(egui::epaint::RectShape::filled(rect, round, tint).with_texture(ramp.id(), uv));
    // Only the top edge of an inside stroke survives this clip -- which is
    // what a 1 px downward inset shadow leaves visible.
    let top = Rect::from_min_max(rect.min, pos2(rect.right(), rect.top() + rect.height() * 0.35));
    painter.with_clip_rect(top.intersect(painter.clip_rect())).rect_stroke(
        rect,
        round,
        Stroke::new(1.0, Color32::from_white_alpha(140)),
        egui::StrokeKind::Inside,
    );
}

/// A colour, darker by `k` (the buttons' ramp and their pressed state).
fn shade(c: Color32, k: f32) -> Color32 {
    let f = |v: u8| (v as f32 * k).round() as u8;
    Color32::from_rgb(f(c.r()), f(c.g()), f(c.b()))
}

fn nearest(pts: &[Pos2], x: f32) -> usize {
    let i = pts.partition_point(|p| p.x < x);
    if i == 0 {
        0
    } else if i >= pts.len() {
        pts.len() - 1
    } else if x - pts[i - 1].x < pts[i].x - x {
        i - 1
    } else {
        i
    }
}

/// The scrub timestamp: the time of day on the short ranges, the date on the
/// long ones, both in between. Robinhood sets it in capitals; this page is
/// lowercase throughout.
fn fmt_moment(t: i64, range: Range) -> String {
    let Some(t) = Local.timestamp_opt(t, 0).single() else { return String::new() };
    let fmt = match range {
        Range::Live | Range::Day => "%-I:%M %p",
        Range::Week | Range::Month => "%b %-d, %-I:%M %p",
        Range::Quarter | Range::Year | Range::FiveYear => "%b %-d, %Y",
    };
    t.format(fmt).to_string()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn iphone_corner_meets_the_edges_late_and_the_circle_at_45_degrees() {
        let (r, rect) = (40.0, Rect::from_min_size(pos2(0.0, 0.0), vec2(340.0, 220.0)));
        let pts = squircle(rect, r);
        // The top-left quarter runs from the left edge to the top edge,
        // leaving each 1.528 r from the corner rather than r.
        assert!((pts[0].x - 0.0).abs() < 1e-3 && (pts[0].y - 1.528 * r).abs() < 1e-3, "{:?}", pts[0]);
        assert!((pts[24].x - 1.528 * r).abs() < 1e-3 && pts[24].y.abs() < 1e-3, "{:?}", pts[24]);
        // Halfway round, it is where a circular corner of radius r is.
        let circle = r * (1.0 - std::f32::consts::FRAC_1_SQRT_2);
        assert!((pts[12].x - circle).abs() < 0.05 && (pts[12].y - circle).abs() < 0.05, "{:?} vs {circle}", pts[12]);
        // Convex, so the fill can be a convex polygon: every turn is the same way.
        let n = pts.len();
        for i in 0..n {
            let (a, b, c) = (pts[i], pts[(i + 1) % n], pts[(i + 2) % n]);
            let cross = (b - a).x * (c - b).y - (b - a).y * (c - b).x;
            assert!(cross >= -1e-3, "turns back at {i}");
        }
    }
}
