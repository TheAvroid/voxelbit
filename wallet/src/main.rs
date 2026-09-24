// No console window behind the app in release builds. --check still prints:
// its output reaches a pipe or file the caller hands it.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use eframe::egui;
use voxelbit_wallet::{app, feed};

fn main() -> eframe::Result {
    if std::env::args().any(|a| a == "--check") {
        std::process::exit(feed::check());
    }

    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_title("voxelbit wallet")
            .with_inner_size([1180.0, 860.0])
            .with_min_inner_size([700.0, 600.0]),
        ..Default::default()
    };
    eframe::run_native("voxelbit wallet", options, Box::new(|cc| Ok(Box::new(app::WalletApp::new(cc)))))
}
