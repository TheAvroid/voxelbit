//! The wordmark composite, written to a PNG on white so it can be laid beside
//! Chrome's render of the website's own CSS and compared.
//!
//!     cargo test --release --test mark -- --ignored
//!
//! Writes mark.png into $WALLET_SHOT_DIR, default target/.

use voxelbit_wallet::mark;

#[test]
#[ignore = "writes a file to look at"]
fn write_the_mark() {
    let m = mark::render(30.0);
    let [w, h] = m.image.size;
    let mut img = image::RgbaImage::new(w as u32, h as u32);
    for (i, p) in m.image.pixels.iter().enumerate() {
        // Straight over white: egui colours are premultiplied.
        let a = p.a() as u32;
        let ch = |c: u8| (c as u32 + (255 - a)).min(255) as u8;
        img.put_pixel((i % w) as u32, (i / w) as u32, image::Rgba([ch(p.r()), ch(p.g()), ch(p.b()), 255]));
    }
    let dir = std::env::var("WALLET_SHOT_DIR").unwrap_or_else(|_| concat!(env!("CARGO_MANIFEST_DIR"), "/target").into());
    let path = format!("{dir}/mark.png");
    img.save(&path).expect("save");
    println!("wrote {path}  {w}x{h}  origin {:?}  width {}", m.origin, m.width);

    let (top, height) = mark::hyphen_em();
    println!("hyphen: top {top} em below the cap line, {height} em tall");

    // If a render of the website's own CSS sits beside it (mark-ref.png:
    // headless Chrome, the wordmark rules verbatim at font-size 30px, on
    // white), align the two on their ink and measure the difference.
    let Ok(reference) = image::open(format!("{dir}/mark-ref.png")) else { return };
    let reference = reference.to_rgb8();
    let ink = |im: &image::RgbImage, thr: u8| {
        let (mut x0, mut y0, mut x1, mut y1) = (u32::MAX, u32::MAX, 0, 0);
        for (x, y, p) in im.enumerate_pixels() {
            if p.0.iter().any(|&c| c < thr) {
                (x0, y0, x1, y1) = (x0.min(x), y0.min(y), x1.max(x), y1.max(y));
            }
        }
        (x0 as i32, y0 as i32, x1 as i32, y1 as i32)
    };
    let mine = image::DynamicImage::ImageRgba8(img).to_rgb8();
    let (r, m) = (ink(&reference, 120), ink(&mine, 120));
    println!("ink box  website {:?} ({}x{})  wallet {:?} ({}x{})", r, r.2 - r.0 + 1, r.3 - r.1 + 1, m, m.2 - m.0 + 1, m.3 - m.1 + 1);
    let (dx, dy) = (r.0 - m.0, r.1 - m.1);
    let get = |im: &image::RgbImage, x: i32, y: i32| {
        if x < 0 || y < 0 || x >= im.width() as i32 || y >= im.height() as i32 { [255, 255, 255] } else { im.get_pixel(x as u32, y as u32).0 }
    };
    let (mut sum, mut worst, mut off, mut n) = (0u64, 0u8, 0, 0);
    let pad = 8;
    let (cw, ch) = ((r.2 - r.0 + 1 + 2 * pad) as u32, (r.3 - r.1 + 1 + 2 * pad) as u32);
    let mut side = image::RgbImage::from_pixel(cw * 4, ch * 8 + 8, image::Rgb([255, 255, 255]));
    for y in 0..ch as i32 {
        for x in 0..cw as i32 {
            let a = get(&reference, r.0 - pad + x, r.1 - pad + y);
            let b = get(&mine, r.0 - pad + x - dx, r.1 - pad + y - dy);
            let d = (0..3).map(|i| a[i].abs_diff(b[i])).max().unwrap();
            sum += d as u64;
            worst = worst.max(d);
            off += (d > 24) as u32;
            n += 1;
            for (sy, sx) in (0..4).flat_map(|i| (0..4).map(move |j| (i, j))) {
                side.put_pixel(x as u32 * 4 + sx, y as u32 * 4 + sy, image::Rgb(a));
                side.put_pixel(x as u32 * 4 + sx, ch * 4 + 8 + y as u32 * 4 + sy, image::Rgb(b));
            }
        }
    }
    println!("compared {n} px: mean diff {:.2}/255, worst {worst}, {off} px off by more than 24", sum as f64 / n as f64);
    side.save(format!("{dir}/mark-compare.png")).expect("save compare");
}
