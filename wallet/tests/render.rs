//! Draws the real window offscreen, fed by the live market, and writes PNGs --
//! so a layout change can be looked at without a window ever opening on the
//! desktop. It needs the network and a GPU, so a plain `cargo test` skips it:
//!
//!     cargo test --release --test render -- --ignored
//!
//! Writes wallet.png, wallet-hover.png (the pointer scrubbing the chart),
//! wallet-1y.png (after clicking 1y) and wallet-narrow.png (the one-column
//! layout, drawn tall enough to show all of it) into $WALLET_SHOT_DIR,
//! default target/.

use std::sync::Arc;
use std::time::{Duration, Instant};

use eframe::egui;
use egui_kittest::Harness;
use egui_kittest::kittest::Queryable;
use voxelbit_wallet::app::{self, WalletApp};
use voxelbit_wallet::feed::{self, Link, Range, Shared};

#[test]
#[ignore = "needs the network and a GPU"]
fn render_live_window() {
    let market: Shared = Default::default();
    feed::spawn(market.clone(), Arc::new(|| {}));
    let t0 = Instant::now();
    loop {
        {
            let m = market.lock().unwrap();
            // The chain tip comes from a second service; wait for it too, or
            // the stats grid renders its market cap as "--".
            if matches!(m.link, Link::Live) && m.height.is_some() {
                break;
            }
        }
        assert!(t0.elapsed() < Duration::from_secs(20), "no live trade and chain tip in 20 s");
        std::thread::sleep(Duration::from_millis(50));
    }

    // The window opens on 1D, the stats want 1Y, and the test clicks 1Y
    // later: fill both the way request_history would.
    let now = chrono::Utc::now().timestamp();
    let agent = feed::http();
    for range in [Range::Day, Range::Year] {
        let mut series = feed::fetch_series(&agent, range, now).expect("history");
        let mut m = market.lock().unwrap();
        if let (Some(p), Some(t)) = (m.price, m.last_trade) {
            series.apply_trade(t, p);
        }
        m.series.insert(range, series);
    }

    let dir = std::env::var("WALLET_SHOT_DIR").unwrap_or_else(|_| concat!(env!("CARGO_MANIFEST_DIR"), "/target").into());
    let size = egui::Vec2::new(1180.0, 860.0);
    let app = WalletApp::with_market(market, Arc::new(|| {}), false);
    let mut harness = Harness::builder()
        .with_size(size)
        .wgpu()
        .build_ui_state(|ui, app: &mut WalletApp| app.draw(ui), app);
    app::style(&harness.ctx);

    // The price rolls on a wall-clock timer and kittest's frames take no
    // time, so a capture straight after a change catches the roll at frame
    // zero -- the OLD price. Let it finish first.
    let save = |harness: &mut Harness<'_, WalletApp>, name: &str| {
        std::thread::sleep(Duration::from_millis(400));
        harness.run_steps(2);
        let shot = format!("{dir}/{name}");
        harness.render().expect("render").save(&shot).expect("save png");
        println!("wrote {shot}");
    };

    // Parked over the top bar, well away from the chart.
    harness.hover_at(egui::pos2(size.x * 0.5, 20.0));
    harness.run_steps(6);
    save(&mut harness, "wallet.png");

    // Scrubbing: the header should show that moment's price, the timestamp
    // should ride the cursor, and the line past it should turn grey.
    harness.hover_at(egui::pos2(size.x * 0.36, 400.0));
    harness.run_steps(6);
    save(&mut harness, "wallet-hover.png");

    harness.hover_at(egui::pos2(size.x * 0.5, 20.0));
    harness.get_by_label("1y").click();
    harness.run_steps(6);
    save(&mut harness, "wallet-1y.png");

    // The buy pills have no wallet behind them yet: a press must say so.
    harness.get_by_label("buy 100 bits").click();
    harness.run_steps(4);
    save(&mut harness, "wallet-buy.png");

    // The pointer over the first pill: it should rise off the card, shadow under it.
    harness.hover_at(egui::pos2(830.0, 239.0));
    harness.run_steps(4);
    save(&mut harness, "wallet-hover-buy.png");

    // The narrowest the window allows: one column, the converter under the
    // stats instead of beside the chart.
    let app = std::mem::replace(harness.state_mut(), WalletApp::with_market(Default::default(), Arc::new(|| {}), false));
    let narrow = egui::Vec2::new(700.0, 1500.0);
    let mut harness = Harness::builder()
        .with_size(narrow)
        .wgpu()
        .build_ui_state(|ui, app: &mut WalletApp| app.draw(ui), app);
    app::style(&harness.ctx);
    harness.hover_at(egui::pos2(narrow.x * 0.5, 20.0));
    harness.run_steps(6);
    save(&mut harness, "wallet-narrow.png");
}
