// SPDX-License-Identifier: GPL-3.0-or-later
//! Makes the PS5's shader pack from a game folder's shader cache (shaders::make_pack), then checks every shader in it against the
//! loose file:  cargo run --release --example shader_pack -- <game dir>
#![allow(dead_code)]
#[path = "../src/shaders.rs"]
mod shaders;

use std::io::{Read, Seek, SeekFrom};
use std::sync::atomic::AtomicBool;
use std::time::Instant;

fn main() {
    let game = std::path::PathBuf::from(std::env::args().nth(1).expect("usage: shader_pack <game dir>"));
    let t0 = Instant::now();
    let mut last = 0;
    let ok = shaders::make_pack(&game, &AtomicBool::new(false), |d, t, _| {
        let pct = if t > 0 { d * 100 / t } else { 0 };
        if pct >= last + 10 {
            println!("{:>5.0}s {}%", t0.elapsed().as_secs_f64(), pct);
            last = pct;
        }
    })
    .unwrap();
    let pack = game.join(shaders::PACK_REL);
    let size = std::fs::metadata(&pack).unwrap().len();
    println!("pack {} in {:.0} s: {:.2} GB", ok, t0.elapsed().as_secs_f64(), size as f64 / 1e9);
    let t1 = Instant::now();
    let mut f = std::fs::File::open(&pack).unwrap();
    let mut head = [0u8; 24];
    f.read_exact(&mut head).unwrap();
    assert_eq!(&head[..8], b"NXSPPAK1");
    let count = u32::from_le_bytes(head[8..12].try_into().unwrap()) as usize;
    let index = u64::from_le_bytes(head[16..24].try_into().unwrap());
    let mut idx = vec![0u8; count * 24];
    f.seek(SeekFrom::Start(index)).unwrap();
    f.read_exact(&mut idx).unwrap();
    let (mut loose, mut bad, mut prev) = (0u64, 0, 0u64);
    for i in 0..count {
        let r = &idx[i * 24..i * 24 + 24];
        let h = u64::from_le_bytes(r[0..8].try_into().unwrap());
        let o = u64::from_le_bytes(r[8..16].try_into().unwrap());
        let z = u32::from_le_bytes(r[16..20].try_into().unwrap()) as usize;
        let s = u32::from_le_bytes(r[20..24].try_into().unwrap()) as usize;
        assert!(i == 0 || h > prev, "index not sorted");
        prev = h;
        let mut buf = vec![0u8; z];
        f.seek(SeekFrom::Start(o)).unwrap();
        f.read_exact(&mut buf).unwrap();
        let out = zstd::bulk::decompress(&buf, s).unwrap();
        let want = std::fs::read(game.join(shaders::SPIRV_REL).join(format!("{:016x}.spv", h))).unwrap();
        loose += want.len() as u64;
        if out != want {
            bad += 1;
        }
    }
    println!("checked {} shaders in {:.0} s: {} differ; loose {:.2} GB -> pack {:.2} GB (x{:.1})", count, t1.elapsed().as_secs_f64(), bad, loose as f64 / 1e9, size as f64 / 1e9, loose as f64 / size as f64);
}
