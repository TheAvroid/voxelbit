//! Amounts the way the wallet will hold them: whole satoshis in an integer.
//!
//! 1 BTC = 100,000,000 sats = 1,000,000 bits, so 1 bit = 100 sats. "Bits" is
//! the unit the owner asked for -- a million of them is one bitcoin -- and it
//! is also the BIP 176 name for the same thing, which is why it divides
//! exactly.
//!
//! A bitcoin amount never lives in an f64 here. A double holds ~15.9
//! significant digits; 21,000,000.00000001 BTC needs 16, so the top of the
//! supply cannot round-trip through one. Parsing goes string -> integer
//! directly. Dollars are the only float, because a price is a quote, not a
//! balance.

pub const SATS_PER_BTC: u64 = 100_000_000;
pub const SATS_PER_BIT: u64 = 100;
pub const BITS_PER_BTC: u64 = SATS_PER_BTC / SATS_PER_BIT;

/// Decimal places each unit can carry before it goes below one satoshi.
pub const BTC_DECIMALS: u32 = 8;
pub const BIT_DECIMALS: u32 = 2;

/// Parse "1,234.56" into an integer count of 10^-`decimals` units, exactly.
///
/// Commas, spaces and a leading `$` are ignored so a pasted amount works.
/// More fraction digits than the unit can hold is an error, not a rounding:
/// 0.001 bits is a tenth of a satoshi and does not exist.
pub fn parse_fixed(text: &str, decimals: u32) -> Option<u64> {
    let clean: String = text
        .trim()
        .trim_start_matches('$')
        .chars()
        .filter(|c| *c != ',' && *c != ' ' && *c != '_')
        .collect();
    if clean.is_empty() {
        return None;
    }
    let (whole, frac) = match clean.split_once('.') {
        Some((w, f)) => (w, f),
        None => (clean.as_str(), ""),
    };
    if whole.is_empty() && frac.is_empty() {
        return None;
    }
    if !whole.chars().all(|c| c.is_ascii_digit()) || !frac.chars().all(|c| c.is_ascii_digit()) {
        return None;
    }
    if frac.len() > decimals as usize {
        return None;
    }
    let scale = 10u64.checked_pow(decimals)?;
    let whole: u64 = if whole.is_empty() { 0 } else { whole.parse().ok()? };
    let mut frac_units: u64 = if frac.is_empty() { 0 } else { frac.parse().ok()? };
    frac_units = frac_units.checked_mul(10u64.pow(decimals - frac.len() as u32))?;
    whole.checked_mul(scale)?.checked_add(frac_units)
}

pub fn parse_btc(text: &str) -> Option<u64> {
    parse_fixed(text, BTC_DECIMALS)
}

/// Bits carry two decimals, and two decimals of a bit are exactly sats.
pub fn parse_bits(text: &str) -> Option<u64> {
    parse_fixed(text, BIT_DECIMALS)
}

pub fn parse_usd(text: &str) -> Option<f64> {
    // Cents, exactly, then to f64 -- so "12.345" is refused rather than
    // silently becoming 12.35.
    parse_fixed(text, 2).map(|cents| cents as f64 / 100.0)
}

/// "1234567" -> "1,234,567".
pub fn group(n: u64) -> String {
    let digits = n.to_string();
    let mut out = String::with_capacity(digits.len() + digits.len() / 3);
    for (i, c) in digits.chars().enumerate() {
        if i > 0 && (digits.len() - i) % 3 == 0 {
            out.push(',');
        }
        out.push(c);
    }
    out
}

/// Sats as BTC with all eight places: "0.00123456".
pub fn fmt_btc(sats: u64) -> String {
    format!("{}.{:08}", group(sats / SATS_PER_BTC), sats % SATS_PER_BTC)
}

/// Sats as bits, dropping ".00" on whole amounts: "1,000,000" / "12.34".
pub fn fmt_bits(sats: u64) -> String {
    let (whole, frac) = (sats / SATS_PER_BIT, sats % SATS_PER_BIT);
    if frac == 0 { group(whole) } else { format!("{}.{:02}", group(whole), frac) }
}

/// Dollars with thousands separators at a fixed number of places.
pub fn fmt_usd(usd: f64, places: usize) -> String {
    if !usd.is_finite() {
        return "--".into();
    }
    let sign = if usd < 0.0 { "-" } else { "" };
    let scale = 10f64.powi(places as i32);
    let scaled = (usd.abs() * scale).round() as u64;
    let unit = 10u64.pow(places as u32);
    let whole = group(scaled / unit);
    if places == 0 {
        format!("{sign}${whole}")
    } else {
        format!("{sign}${whole}.{:0width$}", scaled % unit, width = places)
    }
}

/// Large dollar figures the way a stats grid wants them: "$1.69T", "$712.4M".
pub fn fmt_usd_compact(usd: f64) -> String {
    let a = usd.abs();
    let (v, suffix) = if a >= 1e12 {
        (usd / 1e12, "T")
    } else if a >= 1e9 {
        (usd / 1e9, "B")
    } else if a >= 1e6 {
        (usd / 1e6, "M")
    } else if a >= 1e3 {
        (usd / 1e3, "K")
    } else {
        return fmt_usd(usd, 2);
    };
    let places = if v.abs() >= 100.0 { 1 } else { 2 };
    format!("${v:.places$}{suffix}")
}

/// Same idea for a plain count: "20.09M".
pub fn fmt_compact(n: f64) -> String {
    let a = n.abs();
    let (v, suffix) = if a >= 1e9 {
        (n / 1e9, "B")
    } else if a >= 1e6 {
        (n / 1e6, "M")
    } else if a >= 1e3 {
        (n / 1e3, "K")
    } else {
        return format!("{n:.0}");
    };
    format!("{v:.2}{suffix}")
}

/// Every satoshi the chain has issued once block `height` is mined: 50 BTC a
/// block, halved (integer shift, as consensus does it) every 210,000 blocks.
/// This counts the genesis block's unspendable 50 and coins lost since, as
/// every published "circulating supply" does; it is issuance, not holdings.
pub fn supply_at_height(height: u64) -> u64 {
    const HALVING: u64 = 210_000;
    let mut blocks = height + 1; // heights start at 0
    let mut subsidy = 50 * SATS_PER_BTC;
    let mut total = 0;
    while blocks > 0 && subsidy > 0 {
        let n = blocks.min(HALVING);
        total += n * subsidy;
        blocks -= n;
        subsidy >>= 1;
    }
    total
}

pub fn sats_to_usd(sats: u64, usd_per_btc: f64) -> f64 {
    sats as f64 / SATS_PER_BTC as f64 * usd_per_btc
}

/// Rounded to the nearest satoshi. None for a price that is not a price.
pub fn usd_to_sats(usd: f64, usd_per_btc: f64) -> Option<u64> {
    if !(usd_per_btc > 0.0) || !(usd >= 0.0) {
        return None;
    }
    let sats = (usd / usd_per_btc * SATS_PER_BTC as f64).round();
    // 2^53: past this an f64 stops counting whole sats. It is 43x the supply.
    (sats < 9_007_199_254_740_992.0).then_some(sats as u64)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_million_bits_is_one_bitcoin() {
        assert_eq!(BITS_PER_BTC, 1_000_000);
        assert_eq!(parse_bits("1,000,000"), parse_btc("1"));
        assert_eq!(fmt_bits(SATS_PER_BTC), "1,000,000");
        assert_eq!(fmt_btc(parse_bits("1000000").unwrap()), "1.00000000");
    }

    #[test]
    fn parses_exactly() {
        assert_eq!(parse_btc("0.00000001"), Some(1));
        assert_eq!(parse_btc("21000000.00000001"), Some(2_100_000_000_000_001));
        assert_eq!(parse_btc(".5"), Some(50_000_000));
        assert_eq!(parse_btc("5."), Some(500_000_000));
        assert_eq!(parse_bits("12.34"), Some(1234));
        assert_eq!(parse_usd("$1,234.50"), Some(1234.5));
    }

    #[test]
    fn refuses_what_is_not_an_amount() {
        assert_eq!(parse_bits("0.001"), None, "a tenth of a satoshi");
        assert_eq!(parse_btc("0.000000001"), None);
        assert_eq!(parse_usd("12.345"), None);
        for bad in ["", ".", "abc", "1.2.3", "-1", "1e5", "99999999999999999999"] {
            assert_eq!(parse_btc(bad), None, "{bad:?}");
        }
    }

    #[test]
    fn formats() {
        assert_eq!(group(0), "0");
        assert_eq!(group(999), "999");
        assert_eq!(group(1000), "1,000");
        assert_eq!(group(84182), "84,182");
        assert_eq!(fmt_btc(123_456), "0.00123456");
        assert_eq!(fmt_bits(1234), "12.34");
        assert_eq!(fmt_usd(84182.254, 2), "$84,182.25");
        assert_eq!(fmt_usd(0.0841823, 4), "$0.0842");
        assert_eq!(fmt_usd(-412.3, 2), "-$412.30");
    }

    #[test]
    fn compact() {
        assert_eq!(fmt_usd_compact(1.69e12), "$1.69T");
        assert_eq!(fmt_usd_compact(712_400_000.0), "$712.4M");
        assert_eq!(fmt_usd_compact(12.5), "$12.50");
        assert_eq!(fmt_compact(20_088_418.75), "20.09M");
    }

    #[test]
    fn supply_follows_the_halvings() {
        assert_eq!(supply_at_height(0), 50 * SATS_PER_BTC);
        assert_eq!(supply_at_height(209_999), 10_500_000 * SATS_PER_BTC);
        // 840,000 blocks = four whole eras: 50 + 25 + 12.5 + 6.25 per block.
        assert_eq!(supply_at_height(839_999), 19_687_500 * SATS_PER_BTC);
        // The cap is 20,999,999.9769 BTC, not 21M: the subsidy's low bits
        // shift away before it reaches zero.
        assert_eq!(supply_at_height(10_000_000), 2_099_999_997_690_000);
    }

    #[test]
    fn converts_through_the_price() {
        assert_eq!(usd_to_sats(84_182.25, 84_182.25), Some(SATS_PER_BTC));
        assert_eq!(sats_to_usd(SATS_PER_BTC / 2, 80_000.0), 40_000.0);
        assert_eq!(usd_to_sats(1.0, 0.0), None);
        assert_eq!(usd_to_sats(1.0, f64::NAN), None);
    }
}
