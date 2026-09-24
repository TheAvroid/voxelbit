//! The live BTC/USD price and its history, from Coinbase's public market data.
//!
//! Two sources, because they answer different questions:
//!
//! * The WebSocket ticker pushes every trade as it happens. That is what makes
//!   the number at the top "real time" -- polling a REST quote every few
//!   seconds would miss most of the moves and still cost a request each time.
//! * The REST candles endpoint backfills the chart. A socket only knows what
//!   happened since it connected; the chart needs the last hour/day/year.
//!
//! No API key: both are public market data. Nothing here touches an account.
//!
//! Coinbase Exchange was chosen over Binance because binance.com refuses US
//! connections, and over mempool.space's /prices because that is a
//! once-a-minute index, not a trade feed (checked 2026-09-23). mempool.space
//! IS used for the one thing an exchange cannot say: the chain's height,
//! which is what the supply (and so the market cap) is computed from.

use std::collections::{HashMap, HashSet};
use std::net::TcpStream;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use serde::Deserialize;
use tungstenite::stream::MaybeTlsStream;
use tungstenite::{Message, WebSocket};

pub const PRODUCT: &str = "BTC-USD";
const WS_URL: &str = "wss://ws-feed.exchange.coinbase.com";
const REST_URL: &str = "https://api.exchange.coinbase.com";
const TIP_URL: &str = "https://mempool.space/api/blocks/tip/height";

/// The socket also subscribes to a once-a-second heartbeat, so this long
/// without a byte means the connection is dead, not that the market is quiet.
const READ_TIMEOUT: Duration = Duration::from_secs(10);

/// Coinbase returns at most 300 candles per request; five years of days is
/// 1,826, so 5Y takes seven pages. 290 leaves room for the endpoint counting
/// both ends of the window.
const CANDLES_PER_PAGE: i64 = 290;

/// A chart older than this is fetched again when its range is re-selected.
/// Between fetches, live trades keep the newest point moving.
const HISTORY_MAX_AGE: Duration = Duration::from_secs(10 * 60);
const HISTORY_RETRY: Duration = Duration::from_secs(10);

/// A block is ten minutes on average; asking every five is plenty.
const TIP_EVERY: Duration = Duration::from_secs(5 * 60);

#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub enum Range {
    Live,
    Day,
    Week,
    Month,
    Quarter,
    Year,
    FiveYear,
}

impl Range {
    pub const ALL: [Range; 7] =
        [Range::Live, Range::Day, Range::Week, Range::Month, Range::Quarter, Range::Year, Range::FiveYear];

    pub fn label(self) -> &'static str {
        match self {
            Range::Live => "live",
            Range::Day => "1d",
            Range::Week => "1w",
            Range::Month => "1m",
            Range::Quarter => "3m",
            Range::Year => "1y",
            Range::FiveYear => "5y",
        }
    }

    /// What the change beside the price is measured over.
    pub fn words(self) -> &'static str {
        match self {
            Range::Live => "past hour",
            Range::Day => "past day",
            Range::Week => "past week",
            Range::Month => "past month",
            Range::Quarter => "past 3 months",
            Range::Year => "past year",
            Range::FiveYear => "past 5 years",
        }
    }

    /// Candle size of the fetched history, seconds. Coinbase accepts only
    /// 60, 300, 900, 3600, 21600 and 86400; each range takes the one giving
    /// 60-365 points, except 5Y, which is days because nothing coarser exists.
    pub fn granularity(self) -> i64 {
        match self {
            Range::Live => 60,
            Range::Day => 300,
            Range::Week => 3600,
            Range::Month => 21600,
            Range::Quarter | Range::Year | Range::FiveYear => 86400,
        }
    }

    /// How finely live trades are folded into the chart once it is loaded.
    /// LIVE is the one range meant to be WATCHED, so it takes a point every
    /// five seconds instead of one a minute -- the line visibly moves.
    pub fn live_bucket(self) -> i64 {
        match self {
            Range::Live => 5,
            r => r.granularity(),
        }
    }

    pub fn span(self) -> i64 {
        const DAY: i64 = 86400;
        match self {
            Range::Live => 3600,
            Range::Day => DAY,
            Range::Week => 7 * DAY,
            Range::Month => 30 * DAY,
            Range::Quarter => 91 * DAY,
            Range::Year => 365 * DAY,
            Range::FiveYear => 1826 * DAY,
        }
    }
}

/// One chart: (the moment a price is AT, unix seconds; USD). A fetched
/// candle is placed at its END, so every point reads "the price at this
/// time"; the newest point is the latest trade.
pub struct Series {
    pub range: Range,
    pub points: Vec<(i64, f64)>,
    /// Start of the bucket the newest point belongs to.
    open_bucket: i64,
    pub fetched: Instant,
}

impl Series {
    fn from_candles(range: Range, candles: &[(i64, f64)], now: i64) -> Self {
        let g = range.granularity();
        let points = candles.iter().map(|&(start, close)| ((start + g).min(now), close)).collect();
        let open_bucket = candles.last().map_or(0, |c| c.0);
        Series { range, points, open_bucket, fetched: Instant::now() }
    }

    /// Fold a live trade in: it moves the newest point, or starts a new one
    /// once the trade is past the newest point's bucket.
    pub fn apply_trade(&mut self, t: i64, price: f64) {
        let b = self.range.live_bucket();
        let bucket = t - t.rem_euclid(b);
        if bucket < self.open_bucket {
            // Stamped before our newest point (a late message after a
            // reconnect). That point already has a later price.
            return;
        }
        match self.points.last_mut() {
            Some(last) if bucket == self.open_bucket => *last = (t.max(last.0), price),
            _ => {
                self.points.push((t, price));
                self.open_bucket = bucket;
            }
        }
        let oldest = t - self.range.span();
        let drop = self.points.iter().take_while(|p| p.0 < oldest).count();
        let drop = drop.min(self.points.len().saturating_sub(2));
        self.points.drain(..drop);
    }

    pub fn high_low(&self) -> Option<(f64, f64)> {
        let first = self.points.first()?.1;
        Some(self.points.iter().fold((first, first), |(h, l), p| (h.max(p.1), l.min(p.1))))
    }
}

#[derive(Clone, Debug, Default)]
pub enum Link {
    #[default]
    Connecting,
    Live,
    Down(String),
}

#[derive(Default)]
pub struct Market {
    pub price: Option<f64>,
    pub open_24h: Option<f64>,
    pub high_24h: Option<f64>,
    pub low_24h: Option<f64>,
    /// BTC traded on Coinbase in the last 24 h. Coinbase's alone -- a
    /// fraction of the world's, and labelled as such on screen.
    pub volume_24h: Option<f64>,
    /// Exchange timestamp of the latest trade, unix seconds.
    pub last_trade: Option<i64>,
    /// When the latest trade or heartbeat arrived here.
    pub last_heard: Option<Instant>,
    /// +1 / -1 for the direction of the latest tick, and when it came.
    pub tick: Option<(i8, Instant)>,
    pub link: Link,
    /// The chain's tip, from mempool.space.
    pub height: Option<u64>,
    pub series: HashMap<Range, Series>,
    pub loading: HashSet<Range>,
    pub history_error: Option<String>,
    /// A range whose fetch failed is not asked for again before this. The UI
    /// requests history every frame, so without it one bad response would
    /// become sixty requests a second.
    pub retry_at: HashMap<Range, Instant>,
}

impl Market {
    pub fn apply_trade(&mut self, price: f64, t: i64) {
        if let Some(prev) = self.price {
            if price != prev {
                self.tick = Some((if price > prev { 1 } else { -1 }, Instant::now()));
            }
        }
        self.price = Some(price);
        // The exchange's clock, not ours, so live points line up with candles
        // it served. Never step backwards if messages arrive out of order.
        self.last_trade = Some(self.last_trade.map_or(t, |p| p.max(t)));
        self.last_heard = Some(Instant::now());
        for s in self.series.values_mut() {
            s.apply_trade(t, price);
        }
    }
}

pub type Shared = Arc<Mutex<Market>>;
pub type Wake = Arc<dyn Fn() + Send + Sync>;

pub fn http() -> ureq::Agent {
    use ureq::tls::{RootCerts, TlsConfig, TlsProvider};
    // Windows' own TLS (SChannel) instead of ureq's default rustls: rustls
    // needs a C-compiled crypto backend, SChannel is already on the machine.
    //
    // PlatformVerifier is required, not a preference. ureq's default roots
    // are Mozilla's list handed to SChannel as "user-specified"; SChannel
    // builds Coinbase's chain from the WINDOWS store and then refuses it with
    // "unable to find any user-specified roots in the final cert chain"
    // (every candle request failed that way on 2026-09-23, while the
    // WebSocket -- native-tls defaults, system roots -- connected fine).
    let tls = TlsConfig::builder().provider(TlsProvider::NativeTls).root_certs(RootCerts::PlatformVerifier).build();
    ureq::Agent::config_builder()
        .timeout_global(Some(Duration::from_secs(15)))
        .user_agent(concat!("voxelbit-wallet/", env!("CARGO_PKG_VERSION")))
        .tls_config(tls)
        .build()
        .into()
}

fn now_unix() -> i64 {
    chrono::Utc::now().timestamp()
}

fn iso(t: i64) -> String {
    // A literal Z, not "+00:00": a '+' in a query string decodes as a space.
    chrono::DateTime::from_timestamp(t, 0)
        .unwrap_or_default()
        .format("%Y-%m-%dT%H:%M:%SZ")
        .to_string()
}

fn parse_time(s: &str) -> Option<i64> {
    chrono::DateTime::parse_from_rfc3339(s).ok().map(|d| d.timestamp())
}

fn num(s: &Option<String>) -> Option<f64> {
    s.as_deref()?.parse().ok().filter(|v: &f64| v.is_finite() && *v > 0.0)
}

// ---------------------------------------------------------------- live ticker

#[derive(Deserialize)]
struct WsMessage {
    #[serde(rename = "type")]
    kind: String,
    price: Option<String>,
    open_24h: Option<String>,
    high_24h: Option<String>,
    low_24h: Option<String>,
    volume_24h: Option<String>,
    time: Option<String>,
    message: Option<String>,
    reason: Option<String>,
}

/// Start the background threads: the ticker, and the chain-tip poll. Both
/// run for the life of the process and recover on their own; `wake` is
/// called whenever there is news to draw.
pub fn spawn(market: Shared, wake: Wake) {
    let (m, w) = (market.clone(), wake.clone());
    std::thread::Builder::new()
        .name("ticker".into())
        .spawn(move || ticker_loop(m, w))
        .expect("spawn ticker thread");
    std::thread::Builder::new()
        .name("chain-tip".into())
        .spawn(move || tip_loop(market, wake))
        .expect("spawn chain-tip thread");
}

fn ticker_loop(market: Shared, wake: Wake) {
    let agent = http();
    let mut backoff = Duration::from_secs(1);
    loop {
        let mut heard = false;
        let why = match stream_ticks(&market, &wake, &mut heard) {
            Ok(()) => "closed by the exchange".to_string(),
            Err(e) => e,
        };
        market.lock().unwrap().link = Link::Down(why);
        wake();
        if heard {
            backoff = Duration::from_secs(1);
        }
        // While the socket is down, one REST quote per retry keeps the price
        // on screen honest instead of frozen at the last trade.
        if let Ok(q) = fetch_stats(&agent) {
            let mut m = market.lock().unwrap();
            m.open_24h = Some(q.open);
            m.high_24h = Some(q.high);
            m.low_24h = Some(q.low);
            m.volume_24h = Some(q.volume);
            m.apply_trade(q.last, now_unix());
            drop(m);
            wake();
        }
        std::thread::sleep(backoff);
        backoff = (backoff * 2).min(Duration::from_secs(30));
    }
}

fn set_read_timeout(ws: &mut WebSocket<MaybeTlsStream<TcpStream>>) {
    let tcp: &TcpStream = match ws.get_ref() {
        MaybeTlsStream::Plain(s) => s,
        MaybeTlsStream::NativeTls(s) => s.get_ref(),
        _ => return,
    };
    let _ = tcp.set_read_timeout(Some(READ_TIMEOUT));
}

fn stream_ticks(market: &Shared, wake: &Wake, heard: &mut bool) -> Result<(), String> {
    let (mut ws, _) = tungstenite::connect(WS_URL).map_err(|e| format!("connect: {e}"))?;
    set_read_timeout(&mut ws);
    let subscribe = format!(
        r#"{{"type":"subscribe","product_ids":["{PRODUCT}"],"channels":["ticker","heartbeat"]}}"#
    );
    ws.send(Message::Text(subscribe.into())).map_err(|e| format!("subscribe: {e}"))?;

    loop {
        let text = match ws.read() {
            Ok(Message::Text(t)) => t,
            Ok(Message::Close(_)) => return Ok(()),
            Ok(_) => continue, // pings are answered by tungstenite itself
            Err(tungstenite::Error::Io(e))
                if matches!(e.kind(), std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut) =>
            {
                return Err(format!("no data for {}s", READ_TIMEOUT.as_secs()));
            }
            Err(e) => return Err(e.to_string()),
        };
        let Ok(msg) = serde_json::from_str::<WsMessage>(&text) else { continue };
        let mut m = market.lock().unwrap();
        match msg.kind.as_str() {
            "ticker" => {
                let Some(price) = num(&msg.price) else { continue };
                let t = msg.time.as_deref().and_then(parse_time).unwrap_or_else(now_unix);
                m.open_24h = num(&msg.open_24h).or(m.open_24h);
                m.high_24h = num(&msg.high_24h).or(m.high_24h);
                m.low_24h = num(&msg.low_24h).or(m.low_24h);
                m.volume_24h = num(&msg.volume_24h).or(m.volume_24h);
                m.apply_trade(price, t);
                m.link = Link::Live;
                *heard = true;
            }
            "heartbeat" | "subscriptions" => {
                m.last_heard = Some(Instant::now());
                if !matches!(m.link, Link::Live) && m.price.is_some() {
                    m.link = Link::Live;
                }
            }
            "error" => {
                let why = msg.reason.or(msg.message).unwrap_or_else(|| "error".into());
                return Err(format!("exchange: {why}"));
            }
            _ => continue,
        }
        drop(m);
        wake();
    }
}

fn tip_loop(market: Shared, wake: Wake) {
    let agent = http();
    loop {
        if let Ok(h) = fetch_tip(&agent) {
            market.lock().unwrap().height = Some(h);
            wake();
        }
        std::thread::sleep(TIP_EVERY);
    }
}

// ------------------------------------------------------------------ REST

pub struct Stats {
    pub last: f64,
    pub open: f64,
    pub high: f64,
    pub low: f64,
    pub volume: f64,
}

pub fn fetch_stats(agent: &ureq::Agent) -> Result<Stats, String> {
    #[derive(Deserialize)]
    struct Raw {
        last: Option<String>,
        open: Option<String>,
        high: Option<String>,
        low: Option<String>,
        volume: Option<String>,
    }
    let url = format!("{REST_URL}/products/{PRODUCT}/stats");
    let raw: Raw = agent
        .get(&url)
        .call()
        .map_err(|e| e.to_string())?
        .body_mut()
        .read_json()
        .map_err(|e| e.to_string())?;
    let need = |v: &Option<String>, what: &str| num(v).ok_or(format!("stats: no {what}"));
    Ok(Stats {
        last: need(&raw.last, "last")?,
        open: need(&raw.open, "open")?,
        high: need(&raw.high, "high")?,
        low: need(&raw.low, "low")?,
        volume: need(&raw.volume, "volume")?,
    })
}

pub fn fetch_tip(agent: &ureq::Agent) -> Result<u64, String> {
    let body = agent
        .get(TIP_URL)
        .call()
        .map_err(|e| format!("chain tip: {e}"))?
        .body_mut()
        .read_to_string()
        .map_err(|e| format!("chain tip: {e}"))?;
    body.trim().parse().map_err(|_| format!("chain tip: not a height: {body:?}"))
}

pub fn fetch_series(agent: &ureq::Agent, range: Range, now: i64) -> Result<Series, String> {
    let g = range.granularity();
    let start = now - range.span();
    let mut candles: Vec<(i64, f64)> = Vec::new();
    let mut end = now;
    // Pages walk backwards from now; each response is newest-first.
    while end > start {
        let from = (end - CANDLES_PER_PAGE * g).max(start);
        let url = format!(
            "{REST_URL}/products/{PRODUCT}/candles?granularity={g}&start={}&end={}",
            iso(from),
            iso(end)
        );
        // Each row: [time, low, high, open, close, volume].
        let rows: Vec<[f64; 6]> = agent
            .get(&url)
            .call()
            .map_err(|e| format!("{} history: {e}", range.label()))?
            .body_mut()
            .read_json()
            .map_err(|e| format!("{} history: {e}", range.label()))?;
        candles.extend(rows.iter().map(|r| (r[0] as i64, r[4])));
        end = from;
    }
    candles.sort_by_key(|p| p.0);
    candles.dedup_by_key(|p| p.0);
    candles.retain(|p| p.0 + g >= start && p.1.is_finite() && p.1 > 0.0);
    if candles.len() < 2 {
        return Err(format!("{} history: the exchange returned no candles", range.label()));
    }
    Ok(Series::from_candles(range, &candles, now))
}

/// Fetch a range's history in the background unless it is fresh or already
/// on its way. Safe to call every frame.
pub fn request_history(market: &Shared, range: Range, wake: &Wake) {
    {
        let mut m = market.lock().unwrap();
        let fresh = m.series.get(&range).is_some_and(|s| s.fetched.elapsed() < HISTORY_MAX_AGE);
        let backing_off = m.retry_at.get(&range).is_some_and(|t| Instant::now() < *t);
        if fresh || backing_off || !m.loading.insert(range) {
            return;
        }
    }
    let (market, wake) = (market.clone(), wake.clone());
    std::thread::spawn(move || {
        let result = fetch_series(&http(), range, now_unix());
        let mut m = market.lock().unwrap();
        m.loading.remove(&range);
        match result {
            Ok(mut s) => {
                // Trades that arrived while this was downloading.
                if let (Some(p), Some(t)) = (m.price, m.last_trade) {
                    s.apply_trade(t, p);
                }
                m.series.insert(range, s);
                m.history_error = None;
                m.retry_at.remove(&range);
            }
            Err(e) => {
                m.history_error = Some(e);
                m.retry_at.insert(range, Instant::now() + HISTORY_RETRY);
            }
        }
        drop(m);
        wake();
    });
}

/// `--check`: prove the whole data path works with no window. Connects the
/// socket, waits for a real trade, pulls every chart range and the chain
/// tip, prints what it got. Exit 0 only if all of it arrived.
pub fn check() -> i32 {
    use crate::units;
    let market: Shared = Arc::default();
    spawn(market.clone(), Arc::new(|| {}));

    let t0 = Instant::now();
    let price = loop {
        {
            let m = market.lock().unwrap();
            if let (Link::Live, Some(p)) = (&m.link, m.price) {
                break p;
            }
            if t0.elapsed() > Duration::from_secs(20) {
                println!("FAIL  no live trade in 20 s (link: {:?})", m.link);
                return 1;
            }
        }
        std::thread::sleep(Duration::from_millis(50));
    };
    let m = market.lock().unwrap();
    println!("live  {} BTC-USD  first trade after {} ms", units::fmt_usd(price, 2), t0.elapsed().as_millis());
    if let (Some(o), Some(h), Some(l)) = (m.open_24h, m.high_24h, m.low_24h) {
        println!("24h   open {}  high {}  low {}  change {:+.2}%", units::fmt_usd(o, 2), units::fmt_usd(h, 2), units::fmt_usd(l, 2), (price / o - 1.0) * 100.0);
    }
    if let Some(v) = m.volume_24h {
        println!("vol   {:.0} BTC on Coinbase in 24h = {}", v, units::fmt_usd_compact(v * price));
    }
    drop(m);
    let one_bit = units::sats_to_usd(units::SATS_PER_BIT, price);
    let per_dollar = units::usd_to_sats(1.0, price).unwrap_or(0);
    println!("bits  1 bit = {}  $1 = {} bits  {} bits = {} BTC", units::fmt_usd(one_bit, 2), units::fmt_bits(per_dollar), units::group(units::BITS_PER_BTC), units::fmt_btc(units::SATS_PER_BTC));

    let agent = http();
    let mut ok = true;
    match fetch_tip(&agent) {
        Ok(h) => {
            let supply = units::supply_at_height(h);
            println!("chain height {}  supply {} BTC  market cap {}", units::group(h), units::fmt_btc(supply), units::fmt_usd_compact(units::sats_to_usd(supply, price)));
        }
        Err(e) => {
            println!("FAIL  {e}");
            ok = false;
        }
    }
    for range in Range::ALL {
        let t = Instant::now();
        match fetch_series(&agent, range, now_unix()) {
            Ok(s) => {
                let (hi, lo) = s.high_low().unwrap();
                let span_h = (s.points.last().unwrap().0 - s.points[0].0) as f64 / 3600.0;
                println!(
                    "chart {:<4} {:>5} points over {:>8.1} h  low {}  high {}  ({} ms)",
                    range.label(),
                    s.points.len(),
                    span_h,
                    units::fmt_usd(lo, 0),
                    units::fmt_usd(hi, 0),
                    t.elapsed().as_millis()
                );
            }
            Err(e) => {
                println!("FAIL  {e}");
                ok = false;
            }
        }
    }
    let ticks_before = market.lock().unwrap().last_heard;
    std::thread::sleep(Duration::from_secs(3));
    let m = market.lock().unwrap();
    let still_live = matches!(m.link, Link::Live) && m.last_heard != ticks_before;
    println!("feed  {} after 3 s more (latest {})", if still_live { "still live" } else { "STALLED" }, units::fmt_usd(m.price.unwrap_or(0.0), 2));
    if ok && still_live { 0 } else { 1 }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn series(range: Range, candles: &[(i64, f64)], now: i64) -> Series {
        Series::from_candles(range, candles, now)
    }

    #[test]
    fn candles_sit_at_their_end_and_the_open_one_at_now() {
        let s = series(Range::Day, &[(0, 1.0), (300, 2.0), (600, 3.0)], 700);
        assert_eq!(s.points, vec![(300, 1.0), (600, 2.0), (700, 3.0)]);
    }

    #[test]
    fn a_trade_moves_the_open_point_then_starts_the_next() {
        let mut s = series(Range::Day, &[(0, 1.0), (300, 2.0)], 400);
        s.apply_trade(450, 5.0);
        assert_eq!(s.points.last(), Some(&(450, 5.0)), "same 5-min bucket as the open candle");
        s.apply_trade(610, 6.0);
        assert_eq!(s.points.len(), 3, "600 opens a new bucket");
        s.apply_trade(200, 9.0);
        assert_eq!(s.points.last(), Some(&(610, 6.0)), "a late trade changes nothing");
    }

    #[test]
    fn live_takes_a_point_every_five_seconds() {
        let mut s = series(Range::Live, &[(0, 1.0), (60, 2.0)], 90);
        for t in [121, 123, 126, 131] {
            s.apply_trade(t, t as f64);
        }
        // 121 and 123 share the 120 bucket; 126 and 131 each open one.
        assert_eq!(s.points[2..], [(123, 123.0), (126, 126.0), (131, 131.0)]);
    }

    #[test]
    fn the_window_slides() {
        let mut s = series(Range::Live, &[(0, 1.0), (60, 2.0), (120, 3.0)], 150);
        s.apply_trade(3700, 4.0);
        assert!(s.points.iter().all(|p| p.0 >= 3700 - 3600), "{:?}", s.points);
    }
}
