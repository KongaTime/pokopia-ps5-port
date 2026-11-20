// SPDX-License-Identifier: GPL-3.0-or-later
//! The launcher's PS5 install without the window (the same code as the PS5 page, ps5.rs):
//!   cargo run --release --example ps5_install -- <game dir> <exefs> <romfs> <tools dir> <console ip> [check]
//! <exefs> is the dump's own ExeFS with plain NSOs. "check" only connects to the console and says what is there.
//! Prints a line whenever a step's message changes. Ctrl+C stops it; running it again continues.
#![allow(dead_code)]

#[path = "../src/ftp.rs"]
mod ftp;
#[path = "../src/gamebuild.rs"]
mod gamebuild;
#[path = "../src/ps5.rs"]
mod ps5;
#[path = "../src/shaders.rs"]
mod shaders;

use std::path::PathBuf;
use std::sync::atomic::AtomicBool;
use std::time::Instant;

fn main() {
    let a: Vec<String> = std::env::args().skip(1).collect();
    if a.len() < 5 {
        eprintln!("usage: ps5_install <game dir> <exefs> <romfs> <tools dir> <console ip> [check]");
        std::process::exit(2);
    }
    if a.get(5).map(|s| s.as_str()) == Some("check") {
        match ps5::check_console(&a[4]) {
            Ok(t) => println!("{}", t),
            Err(e) => {
                eprintln!("error: {}", e);
                std::process::exit(1)
            }
        }
        return;
    }
    let exefs = PathBuf::from(&a[1]);
    let job = ps5::Job {
        game_dir: PathBuf::from(&a[0]),
        tools_dir: PathBuf::from(&a[3]),
        exefs: exefs.clone(),
        dump_exefs: exefs.clone(),
        romfs: PathBuf::from(&a[2]),
        main: exefs.join("main"),
        host: a[4].clone(),
        jobs: std::thread::available_parallelism().map(|n| n.get()).unwrap_or(4),
    };
    let cancel = AtomicBool::new(false);
    let t0 = Instant::now();
    let mut last = String::new();
    let r = ps5::run(&job, &cancel, |s| {
        for st in s.steps.iter().filter(|x| x.state == "running" || x.state == "paused") {
            let line = format!("[{}] {}", st.id, st.message);
            if line != last {
                println!("{:>7.0}s {}", t0.elapsed().as_secs_f64(), line);
                last = line;
            }
        }
    });
    match r {
        Ok(true) => println!("done in {:.0} s", t0.elapsed().as_secs_f64()),
        Ok(false) => println!("paused"),
        Err(e) => {
            eprintln!("error: {}", e);
            std::process::exit(1)
        }
    }
}
