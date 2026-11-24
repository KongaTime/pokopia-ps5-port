// SPDX-License-Identifier: GPL-3.0-or-later
//! The launcher's .nup reader (nup.rs) without the window:
//!   cargo run --release --example nup_unpack -- <file.nup> <title id> [<out dir>]
//! Says what the package holds; with <out dir>, unpacks it there (Ctrl+C stops it; running it again continues).
#![allow(dead_code)]
#[path = "../src/nup.rs"]
mod nup;

use std::path::Path;
use std::time::Instant;

fn main() {
    let a: Vec<String> = std::env::args().skip(1).collect();
    if a.len() < 2 {
        eprintln!("usage: nup_unpack <file.nup> <title id> [<out dir>]");
        std::process::exit(2);
    }
    let fail = |e: String| -> ! {
        eprintln!("error: {}", e);
        std::process::exit(1)
    };
    let i = nup::info(Path::new(&a[0]), &a[1], "main").unwrap_or_else(|e| fail(e));
    println!("program: {} (version {}), build id {}, romfs {} files / {} bytes, {} ZBIC module(s)", i.dir, i.version, i.build_id, i.files, i.bytes, i.zbic_modules);
    i.notes.iter().for_each(|n| println!("note: {}", n));
    let Some(out) = a.get(2) else { return };
    let t0 = Instant::now();
    let mut last = 0u64;
    let done = nup::extract(
        Path::new(&a[0]),
        Path::new(out),
        &mut |d, t, f| {
            let pct = if t > 0 { d * 100 / t } else { 0 };
            if pct != last {
                println!("{:>6.0}s {:>3}% {}", t0.elapsed().as_secs_f64(), pct, f);
                last = pct;
            }
        },
        &|| false,
    )
    .unwrap_or_else(|e| fail(e));
    println!("{} in {:.0} s", if done { "done" } else { "paused" }, t0.elapsed().as_secs_f64());
}
