// SPDX-License-Identifier: GPL-3.0-or-later
//! The launcher's PS5 install without the window (the same code as the PS5 page, ps5.rs):
//!   cargo run --release --example ps5_install -- <game dir> <exefs> <romfs> <tools dir> <console ip> [check]
//! <exefs> is the dump's own ExeFS with plain NSOs. "check" only connects to the console and says what is there.
//!   cargo run --release --example ps5_install -- upload-test <local dir> <console ip> <remote dir under /data/homebrew>
//! uploads a folder to a scratch folder with the install's own uploader (four connections, size-based skip), uploads it again
//! (nothing must be sent), checks every size, then deletes the scratch folder.
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
    if a.first().map(|s| s.as_str()) == Some("upload-test") && a.len() == 4 {
        upload_test(&a[1], &a[2], &a[3]);
        return;
    }
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

fn upload_test(local: &str, host: &str, remote: &str) {
    let fail = |e: String| -> ! {
        eprintln!("error: {}", e);
        std::process::exit(1)
    };
    if !remote.starts_with("/data/homebrew/") || remote.contains("PPSA") || remote.matches('/').count() != 3 {
        fail("the scratch folder must be /data/homebrew/<name>, not a title folder".into());
    }
    let cancel = AtomicBool::new(false);
    let pairs = vec![(PathBuf::from(local), remote.to_string())];
    let t0 = Instant::now();
    let sent = ps5::sync(host, &pairs, &[], &cancel, |_, _, _| {}).unwrap_or_else(|e| fail(e)).unwrap_or(0);
    println!("first upload: {} files in {:.1} s", sent, t0.elapsed().as_secs_f64());
    let again = ps5::sync(host, &pairs, &[], &cancel, |_, _, _| {}).unwrap_or_else(|e| fail(e)).unwrap_or(0);
    println!("second upload: {} files (0 expected)", again);
    let mut f = ftp::Ftp::open(host, ps5::FTP_PORT).unwrap_or_else(|e| fail(e));
    let have = f.list(remote).unwrap_or_else(|e| fail(e));
    let mut bad = 0;
    let mut n = 0;
    for e in std::fs::read_dir(local).unwrap().flatten() {
        n += 1;
        let name = e.file_name().to_string_lossy().to_string();
        let size = e.metadata().unwrap().len();
        if have.iter().find(|h| h.name == name).map(|h| h.size) != Some(size) {
            bad += 1;
        }
    }
    println!("sizes: {} of {} match", n - bad, n);
    for h in &have {
        let _ = f.dele(&format!("{}/{}", remote, h.name));
    }
    let removed = f.rmd(remote).unwrap_or(false);
    let left = f.list(remote).map(|l| l.len()).unwrap_or(0);
    println!("scratch folder removed: {} ({} entries left)", removed, left);
    f.quit();
    if bad > 0 || again > 0 {
        std::process::exit(1);
    }
}
