// SPDX-License-Identifier: GPL-3.0-or-later
//! PS5 mode: puts the game on the player's own PS5 as the homebrew title PPSA90006, from their own dump, in four steps:
//!   1. build: tools/build/ps5_build.py builds and signs the title on this PC with the bundled PS5 toolchain (tools/ps5) into
//!      `<game>/ps5/PPSA90006` (verify, lift, compile, link, sign; resumable, `<game>/ps5/build/cancel` pauses it);
//!   2. shaders: the PC's own shader setup (shaders.rs) makes `<game>/shader_cache/spirv`, the cache the console reads (it cannot
//!      translate shaders itself); a finished PC setup is reused as it is;
//!   3. game data: the dump's exefs/ and romfs/ to `/data/homebrew/PPSA90006/{exefs,romfs}` over FTP;
//!   4. shaders and title: the shader cache to `shader_cache/spirv` (results.tsv, its index, last), then the title folder (eboot.bin
//!      last: the title is not launchable until it is there).
//! Uploads skip every file the console already has with the same size, so a stopped upload goes on where it was, and run on a few
//! FTP connections at once. `<game>/ps5/install.json` records the finished uploads (per console address), so a finished step is
//! skipped without listing the console again. After an upload that changed anything, `pkp-prepared.txt` on the console is deleted:
//! the title's Prepare game then checks the files again (title_screen.cpp).

use crate::{ftp::Ftp, gamebuild, shaders};
use serde::{Deserialize, Serialize};
use std::collections::{HashMap, HashSet};
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

pub const TITLE_ID: &str = "PPSA90006";
pub const FTP_PORT: u16 = 2121;
const REMOTE: &str = "/data/homebrew/PPSA90006";
/// done.json "format" of ps5_build.py (FORMAT there)
const BUILD_FORMAT: u64 = 1;
/// FTP connections used at once
const WORKERS: usize = 4;

pub const STEPS: [(&str, &str); 4] =
    [("build", "Build the PS5 title"), ("shaders", "Make the shader cache"), ("data", "Upload the game data"), ("upload", "Upload the shaders and the title")];

#[derive(Serialize, Clone, Default)]
pub struct Step {
    pub id: String,
    pub label: String,
    /// "pending", "running", "done", "paused", "error"
    pub state: String,
    pub done: u64,
    pub total: u64,
    pub message: String,
}

#[derive(Serialize, Clone, Default)]
pub struct Status {
    pub running: bool,
    pub steps: Vec<Step>,
    /// "", "done", "paused", "error"
    pub outcome: String,
    pub message: String,
}

#[derive(Serialize, Deserialize, Clone, Default)]
#[serde(default)]
struct Marker {
    /// console address -> the romfs whose game data it has
    data: HashMap<String, String>,
    /// console address -> results.tsv size of the uploaded shader cache, and the eboot.bin sha256 it has
    shaders: HashMap<String, u64>,
    title: HashMap<String, String>,
}

pub fn root(game_dir: &Path) -> PathBuf {
    game_dir.join("ps5")
}

fn marker_path(game_dir: &Path) -> PathBuf {
    root(game_dir).join("install.json")
}

fn read_marker(game_dir: &Path) -> Marker {
    fs::read_to_string(marker_path(game_dir)).ok().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or_default()
}

fn write_marker(game_dir: &Path, m: &Marker) -> Result<(), String> {
    fs::create_dir_all(root(game_dir)).map_err(|e| e.to_string())?;
    fs::write(marker_path(game_dir), serde_json::to_string_pretty(m).unwrap_or_default()).map_err(|e| format!("cannot write {}: {}", marker_path(game_dir).display(), e))
}

fn done_json(game_dir: &Path) -> Option<serde_json::Value> {
    fs::read_to_string(root(game_dir).join("build").join("done.json")).ok().and_then(|t| serde_json::from_str(&t).ok())
}

/// The title is built by this release's toolchain and in place.
pub fn is_built(game_dir: &Path) -> bool {
    root(game_dir).join(TITLE_ID).join("eboot.bin").is_file() && done_json(game_dir).map_or(false, |d| d.get("format").and_then(|v| v.as_u64()) == Some(BUILD_FORMAT))
}

fn eboot_sha(game_dir: &Path) -> String {
    done_json(game_dir).and_then(|d| d.get("eboot_sha256").and_then(|v| v.as_str()).map(String::from)).unwrap_or_default()
}

/// The package has the PS5 toolchain.
pub fn available(tools_dir: &Path) -> bool {
    tools_dir.join("build").join("ps5_build.py").is_file() && tools_dir.join("ps5").join("recipe.json").is_file()
}

fn find_build_tools(tools_dir: &Path) -> Result<(PathBuf, PathBuf), String> {
    let python = tools_dir.join("python").join("python.exe");
    let script = tools_dir.join("build").join("ps5_build.py");
    let mut missing = Vec::new();
    for p in [&python, &script, &tools_dir.join("ps5").join("recipe.json"), &tools_dir.join("build").join("game_build.py")] {
        if !p.is_file() {
            missing.push(p.display().to_string());
        }
    }
    if !missing.is_empty() {
        return Err(format!("The PS5 build cannot run, missing: {}. Extract the release zip again so tools/ is complete.", missing.join("; ")));
    }
    Ok((python, script))
}

/// A console address as typed: an IPv4 address or a host name, no port.
pub fn clean_host(host: &str) -> Result<String, String> {
    let h = host.trim().trim_start_matches("ftp://").trim_end_matches('/');
    let h = h.split(':').next().unwrap_or("").trim();
    if h.is_empty() || h.contains(char::is_whitespace) {
        return Err("Enter your PS5's IP address (Settings, Network, Connection status on the console), for example 192.168.1.20.".into());
    }
    Ok(h.to_string())
}

/// Connects to the console's FTP server and reads the title folder: what the page shows before the install.
pub fn check_console(host: &str) -> Result<String, String> {
    let host = clean_host(host)?;
    let mut f = Ftp::open(&host, FTP_PORT).map_err(|e| {
        format!("No FTP server answers at {}:{} ({}). Start the FTP server on the PS5 (your homebrew tools have one) and check the address.", host, FTP_PORT, e)
    })?;
    let have = f.list(REMOTE)?;
    f.quit();
    let names: HashSet<String> = have.iter().map(|e| e.name.clone()).collect();
    Ok(if names.contains("eboot.bin") {
        format!("Connected to {}. {} is already on this PS5; the install updates it.", host, TITLE_ID)
    } else {
        format!("Connected to {}. {} is not on this PS5 yet.", host, TITLE_ID)
    })
}

/// One console the network scan found.
#[derive(Serialize, Clone)]
pub struct Found {
    pub host: String,
    /// PPSA90006 is already in /data/homebrew there
    pub installed: bool,
}

/// This PC's IPv4 address on its main network: the address a UDP socket "connected" to a public address would send from
/// (connecting a UDP socket sends nothing).
fn local_ipv4() -> Option<std::net::Ipv4Addr> {
    let s = std::net::UdpSocket::bind("0.0.0.0:0").ok()?;
    s.connect("8.8.8.8:80").ok()?;
    match s.local_addr().ok()?.ip() {
        std::net::IpAddr::V4(a) if !a.is_loopback() && !a.is_unspecified() => Some(a),
        _ => None,
    }
}

/// Finds PS5s on this PC's /24 network: every address whose port 2121 answers is asked for its root folder, and the ones that
/// list the console's own folders (system_ex, preinst) count. About two seconds; reads only.
pub fn discover() -> Result<Vec<Found>, String> {
    let me = local_ipv4().ok_or("This PC has no IPv4 network address. Connect it to the same network as the PS5.")?;
    let o = me.octets();
    let hosts: Vec<String> = (1..255u8).filter(|&d| d != o[3]).map(|d| format!("{}.{}.{}.{}", o[0], o[1], o[2], d)).collect();
    let open = Arc::new(Mutex::new(Vec::new()));
    let queue = Arc::new(Mutex::new(hosts));
    let mut workers = Vec::new();
    for _ in 0..64 {
        let (queue, open) = (queue.clone(), open.clone());
        workers.push(std::thread::spawn(move || loop {
            let Some(h) = queue.lock().unwrap().pop() else { break };
            if let Ok(sa) = format!("{}:{}", h, FTP_PORT).parse::<std::net::SocketAddr>() {
                if std::net::TcpStream::connect_timeout(&sa, Duration::from_millis(400)).is_ok() {
                    open.lock().unwrap().push(h);
                }
            }
        }));
    }
    for w in workers {
        let _ = w.join();
    }
    let mut out = Vec::new();
    let mut cands = open.lock().unwrap().clone();
    cands.sort_by_key(|h| h.rsplit('.').next().and_then(|d| d.parse::<u8>().ok()).unwrap_or(0));
    for h in cands {
        let Ok(mut f) = Ftp::open(&h, FTP_PORT) else { continue };
        let root: HashSet<String> = f.list("/").unwrap_or_default().into_iter().map(|e| e.name).collect();
        if root.contains("system_ex") && root.contains("preinst") {
            let installed = f.list(REMOTE).map(|l| l.iter().any(|e| e.name == "eboot.bin")).unwrap_or(false);
            out.push(Found { host: h, installed });
        }
        f.quit();
    }
    Ok(out)
}

/// What each step looks like before (or between) runs, from the files on this PC.
pub fn snapshot(game_dir: &Path, host: &str) -> Vec<Step> {
    let m = read_marker(game_dir);
    let host = clean_host(host).unwrap_or_default();
    let spirv = game_dir.join(shaders::SPIRV_REL);
    let done = [
        is_built(game_dir),
        shaders::is_complete(game_dir),
        !host.is_empty() && m.data.contains_key(&host),
        !host.is_empty() && m.title.get(&host).map_or(false, |s| *s == eboot_sha(game_dir)) && m.shaders.get(&host) == fs::metadata(spirv.join("results.tsv")).ok().map(|x| x.len()).as_ref(),
    ];
    STEPS
        .iter()
        .zip(done)
        .map(|((id, label), d)| Step { id: id.to_string(), label: label.to_string(), state: if d { "done" } else { "pending" }.into(), done: d as u64, total: 1, message: String::new() })
        .collect()
}

pub struct Job {
    pub game_dir: PathBuf,
    pub tools_dir: PathBuf,
    /// the ExeFS the build reads (plain NSOs)
    pub exefs: PathBuf,
    /// the dump's own ExeFS, uploaded as it is (the runtime reads ZBIC itself)
    pub dump_exefs: PathBuf,
    pub romfs: PathBuf,
    /// The player's .nup, when the dump came as one: the console gets that one file (in dump/) instead of exefs/ and romfs/, and
    /// reads the game from it in place.
    pub nup: Option<PathBuf>,
    /// exefs/main as the shader extractor reads it (a plain NSO)
    pub main: PathBuf,
    pub host: String,
    pub jobs: usize,
}

/// Runs the steps that are not done. Ok(true) = all done, Ok(false) = paused. `emit` gets the whole status at every change.
pub fn run(job: &Job, cancel: &AtomicBool, mut emit: impl FnMut(&Status)) -> Result<bool, String> {
    let host = clean_host(&job.host)?;
    let mut st = Status { running: true, steps: snapshot(&job.game_dir, &host), ..Default::default() };
    let set = |st: &mut Status, i: usize, state: &str, done: u64, total: u64, msg: String| {
        st.steps[i] = Step { state: state.into(), done, total, message: msg, ..st.steps[i].clone() };
    };
    emit(&st);
    // check the console first: nothing is worth building when it cannot be reached
    check_console(&host)?;

    // 1. the title
    if !is_built(&job.game_dir) {
        let (python, script) = find_build_tools(&job.tools_dir)?;
        let build = root(&job.game_dir).join("build");
        let ok = gamebuild::run_script(&python, &script, &job.exefs, &job.game_dir, job.jobs, &build, cancel, |t| {
            set(&mut st, 0, "running", t.done, t.total, t.message);
            emit(&st);
        })?;
        if !ok {
            set(&mut st, 0, "paused", 0, 0, "Paused.".into());
            emit(&st);
            return Ok(false);
        }
        let note = if done_json(&job.game_dir).and_then(|d| d.get("matches_reference").and_then(|v| v.as_bool())) == Some(true) {
            "Built and signed (identical to the release's reference build)."
        } else {
            "Built and signed."
        };
        set(&mut st, 0, "done", 1, 1, note.into());
        emit(&st);
    }

    // 2. the shader cache (the PC's own, in the game folder)
    if !shaders::is_complete(&job.game_dir) {
        let tools = shaders::find_tools(&job.tools_dir)?;
        let sj = shaders::Job { game_dir: job.game_dir.clone(), romfs: job.romfs.clone(), main: job.main.clone(), jobs: 0, limit: 0 };
        let ok = shaders::run(&sj, &tools, cancel, |p| {
            set(&mut st, 1, "running", p.done, p.total, p.message.clone());
            emit(&st);
        })?;
        if !ok {
            set(&mut st, 1, "paused", 0, 0, "Paused.".into());
            emit(&st);
            return Ok(false);
        }
        set(&mut st, 1, "done", 1, 1, "Shader cache ready.".into());
        emit(&st);
    }

    let mut m = read_marker(&job.game_dir);
    let romfs_key = job.nup.as_ref().unwrap_or(&job.romfs).to_string_lossy().to_string();
    let mut changed = false;

    // 3. game data: the .nup as it is, or the dump's exefs/ and romfs/
    if m.data.get(&host) != Some(&romfs_key) {
        let pairs = match &job.nup {
            Some(nup) => vec![(nup.clone(), format!("{}/dump/{}", REMOTE, nup.file_name().map(|n| n.to_string_lossy().to_string()).unwrap_or_else(|| "game.nup".into())))],
            None => vec![(job.dump_exefs.clone(), format!("{}/exefs", REMOTE)), (job.romfs.clone(), format!("{}/romfs", REMOTE))],
        };
        let r = sync(&host, &pairs, &[], cancel, |done, total, msg| {
            set(&mut st, 2, "running", done, total, msg);
            emit(&st);
        })?;
        match r {
            None => {
                set(&mut st, 2, "paused", 0, 0, "Paused. Continue goes on where it stopped.".into());
                emit(&st);
                return Ok(false);
            }
            Some(n) => changed |= n > 0,
        }
        m.data.insert(host.clone(), romfs_key);
        write_marker(&job.game_dir, &m)?;
        set(&mut st, 2, "done", 1, 1, "The game data is on the PS5.".into());
        emit(&st);
    }

    // 4. shaders (packed into one file), then the title (eboot.bin last)
    let spirv = job.game_dir.join(shaders::SPIRV_REL);
    let results = spirv.join("results.tsv");
    let ok = shaders::make_pack(&job.game_dir, cancel, |done, total, msg| {
        set(&mut st, 3, "running", done, total, msg);
        emit(&st);
    })?;
    if !ok {
        set(&mut st, 3, "paused", 0, 0, "Paused.".into());
        emit(&st);
        return Ok(false);
    }
    let results_len = fs::metadata(&results).map(|x| x.len()).map_err(|_| format!("{} is missing", results.display()))?;
    let eboot = eboot_sha(&job.game_dir);
    if m.shaders.get(&host) != Some(&results_len) || m.title.get(&host) != Some(&eboot) {
        let title = root(&job.game_dir).join(TITLE_ID);
        let mut last: Vec<(PathBuf, String)> = vec![(results.clone(), format!("{}/shader_cache/spirv/results.tsv", REMOTE))];
        for order in [job.game_dir.join("shader_order.txt"), job.tools_dir.join("shaders").join("shader_order.txt")] {
            if order.is_file() {
                last.push((order, format!("{}/shader_order.txt", REMOTE)));
                break;
            }
        }
        // the title folder, eboot.bin as the very last file
        let mut title_files = Vec::new();
        walk(&title, &title, &mut title_files);
        title_files.sort_by_key(|(rel, _)| rel == "eboot.bin");
        for (rel, _) in &title_files {
            last.push((title.join(rel), format!("{}/{}", REMOTE, rel)));
        }
        let pairs = vec![(job.game_dir.join(shaders::PACK_REL), format!("{}/shader_cache/spirv/spirv.pack", REMOTE))];
        let r = sync(&host, &pairs, &last, cancel, |done, total, msg| {
            set(&mut st, 3, "running", done, total, msg);
            emit(&st);
        })?;
        match r {
            None => {
                set(&mut st, 3, "paused", 0, 0, "Paused. Continue goes on where it stopped.".into());
                emit(&st);
                return Ok(false);
            }
            Some(n) => changed |= n > 0,
        }
        // the console does not let a running title's eboot.bin be replaced, and says nothing: check it. Its file system reports a
        // signed file's size as that of the ELF inside (ps5_build.py records it as eboot_elf_size).
        let mut f = Ftp::open(&host, FTP_PORT)?;
        let file = fs::metadata(title.join("eboot.bin")).map(|x| x.len()).unwrap_or(0);
        let elf = done_json(&job.game_dir).and_then(|d| d.get("eboot_elf_size").and_then(|v| v.as_u64()));
        let got = f.size(&format!("{}/eboot.bin", REMOTE))?;
        if got != Some(file) && (elf.is_none() || got != elf) {
            f.quit();
            return Err(format!("The PS5 did not take the new eboot.bin ({} bytes there, {} expected). Close {} on the console (it cannot be replaced while it runs), then Continue.", got.unwrap_or(0), elf.unwrap_or(file), TITLE_ID));
        }
        if changed {
            let _ = f.dele(&format!("{}/pkp-prepared.txt", REMOTE));
        }
        f.quit();
        m.shaders.insert(host.clone(), results_len);
        m.title.insert(host.clone(), eboot);
        write_marker(&job.game_dir, &m)?;
        set(&mut st, 3, "done", 1, 1, "The shaders and the title are on the PS5.".into());
        emit(&st);
    }
    st.running = false;
    st.outcome = "done".into();
    emit(&st);
    Ok(true)
}

/// Files under `dir` as (relative path with '/', size).
fn walk(base: &Path, dir: &Path, out: &mut Vec<(String, u64)>) {
    let Ok(rd) = fs::read_dir(dir) else { return };
    for e in rd.flatten() {
        let p = e.path();
        let Ok(md) = e.metadata() else { continue };
        if md.is_dir() {
            walk(base, &p, out);
        } else if md.is_file() {
            let rel = p.strip_prefix(base).unwrap_or(&p).to_string_lossy().replace('\\', "/");
            if !rel.ends_with(".part") && !rel.ends_with(".tmp") {
                out.push((rel, md.len()));
            }
        }
    }
}

fn gb(b: u64) -> String {
    format!("{:.1} GB", b as f64 / 1e9)
}

/// Uploads each local folder to its remote folder (or a local file to that remote file), skipping files the console already has
/// with the same size, on WORKERS connections; then `last` (always sent, in order, on one connection). Some(files sent) = done,
/// None = cancelled.
pub fn sync(host: &str, pairs: &[(PathBuf, String)], last: &[(PathBuf, String)], cancel: &AtomicBool, mut report: impl FnMut(u64, u64, String)) -> Result<Option<u64>, String> {
    report(0, 0, "Checking what is already on the PS5...".into());
    let mut todo: Vec<(PathBuf, String, u64)> = Vec::new();
    let mut ctrl = Ftp::open(host, FTP_PORT)?;
    let mut dirs: HashSet<String> = HashSet::new();
    let mut listed = 0usize;
    for (local, remote) in pairs {
        if local.is_file() {
            let (rdir, name) = remote.rsplit_once('/').ok_or("bad remote path")?;
            let size = fs::metadata(local).map(|m| m.len()).unwrap_or(0);
            let have = ctrl.list(rdir)?.into_iter().find(|e| !e.dir && e.name == name).map(|e| e.size);
            if have != Some(size) {
                todo.push((local.clone(), remote.clone(), size));
                dirs.insert(rdir.to_string());
            }
            continue;
        }
        if !local.is_dir() {
            return Err(format!("{} does not exist", local.display()));
        }
        let mut files = Vec::new();
        walk(local, local, &mut files);
        files.sort();
        // remote sizes, one LIST per folder that has local files
        let mut by_dir: HashMap<String, Vec<(String, u64)>> = HashMap::new();
        for (rel, size) in files {
            let (d, name) = match rel.rfind('/') {
                Some(i) => (rel[..i].to_string(), rel[i + 1..].to_string()),
                None => (String::new(), rel.clone()),
            };
            by_dir.entry(d).or_default().push((name, size));
        }
        let mut keys: Vec<String> = by_dir.keys().cloned().collect();
        keys.sort();
        for d in keys {
            if cancel.load(Ordering::SeqCst) {
                return Ok(None);
            }
            let rdir = if d.is_empty() { remote.clone() } else { format!("{}/{}", remote, d) };
            let have: HashMap<String, u64> = ctrl.list(&rdir)?.into_iter().filter(|e| !e.dir).map(|e| (e.name, e.size)).collect();
            listed += 1;
            if listed % 50 == 0 {
                report(0, 0, format!("Checking what is already on the PS5 ({} folders)...", listed));
            }
            for (name, size) in &by_dir[&d] {
                if have.get(name) != Some(size) {
                    let ldir = if d.is_empty() { local.clone() } else { local.join(&d) };
                    todo.push((ldir.join(name), format!("{}/{}", rdir, name), *size));
                    dirs.insert(rdir.clone());
                }
            }
        }
    }
    let mut dl: Vec<String> = dirs.into_iter().collect();
    for (_, r) in last {
        dl.push(r.rsplit_once('/').map(|x| x.0.to_string()).unwrap_or_default());
    }
    dl.sort();
    dl.dedup();
    let mut made: HashSet<String> = HashSet::new();
    for d in &dl {
        if !made.contains(d) {
            ctrl.mkdirs(d);
            let mut cur = String::new();
            for part in d.split('/').filter(|p| !p.is_empty()) {
                cur.push('/');
                cur.push_str(part);
                made.insert(cur.clone());
            }
        }
    }
    ctrl.quit();

    let last_bytes: u64 = last.iter().map(|(l, _)| fs::metadata(l).map(|x| x.len()).unwrap_or(0)).sum();
    let total = todo.iter().map(|t| t.2).sum::<u64>() + last_bytes;
    let nfiles = todo.len() as u64 + last.len() as u64;
    let sent = Arc::new(AtomicU64::new(0));
    let files_done = Arc::new(AtomicU64::new(0));
    let queue = Arc::new(Mutex::new(todo));
    let error: Arc<Mutex<Option<String>>> = Arc::new(Mutex::new(None));
    let stop = Arc::new(AtomicBool::new(false));
    let mut handles = Vec::new();
    let queued = queue.lock().unwrap().len();
    let n_workers = WORKERS.min(queued);
    for _ in 0..n_workers {
        let (queue, sent, files_done, error, stop, host) = (queue.clone(), sent.clone(), files_done.clone(), error.clone(), stop.clone(), host.to_string());
        handles.push(std::thread::spawn(move || {
            let mut conn: Option<Ftp> = None;
            loop {
                if stop.load(Ordering::SeqCst) {
                    break;
                }
                let Some((local, remote, size)) = queue.lock().unwrap().pop() else { break };
                let mut tries = 0;
                loop {
                    if conn.is_none() {
                        match Ftp::open(&host, FTP_PORT) {
                            Ok(c) => conn = Some(c),
                            Err(e) => {
                                tries += 1;
                                if tries >= 4 {
                                    *error.lock().unwrap() = Some(e);
                                    stop.store(true, Ordering::SeqCst);
                                    return;
                                }
                                std::thread::sleep(Duration::from_secs(2));
                                continue;
                            }
                        }
                    }
                    let mut mine = 0u64;
                    let r = conn.as_mut().unwrap().stor(&local, &remote, &stop, |n| {
                        mine += n;
                        sent.fetch_add(n, Ordering::SeqCst);
                    });
                    match r {
                        Ok(true) => {
                            files_done.fetch_add(1, Ordering::SeqCst);
                            break;
                        }
                        Ok(false) => return,
                        Err(e) => {
                            sent.fetch_sub(mine, Ordering::SeqCst);
                            conn = None;
                            tries += 1;
                            if tries >= 4 {
                                *error.lock().unwrap() = Some(format!("{} ({} bytes): {}", remote, size, e));
                                stop.store(true, Ordering::SeqCst);
                                return;
                            }
                            std::thread::sleep(Duration::from_secs(2));
                        }
                    }
                }
            }
            if let Some(c) = conn {
                c.quit();
            }
        }));
    }
    let msg = |s: u64, f: u64| format!("Uploading to the PS5: {} of {} files, {} of {}", f, nfiles, gb(s), gb(total));
    while handles.iter().any(|h| !h.is_finished()) {
        if cancel.load(Ordering::SeqCst) {
            stop.store(true, Ordering::SeqCst);
        }
        report(sent.load(Ordering::SeqCst), total, msg(sent.load(Ordering::SeqCst), files_done.load(Ordering::SeqCst)));
        std::thread::sleep(Duration::from_millis(500));
    }
    for h in handles {
        let _ = h.join();
    }
    if let Some(e) = error.lock().unwrap().take() {
        return Err(format!("Upload to the PS5 failed: {}", e));
    }
    if cancel.load(Ordering::SeqCst) {
        return Ok(None);
    }
    if !last.is_empty() {
        let mut f = Ftp::open(host, FTP_PORT)?;
        for (local, remote) in last {
            let ok = f.stor(local, remote, cancel, |n| {
                sent.fetch_add(n, Ordering::SeqCst);
            })?;
            if !ok {
                return Ok(None);
            }
            files_done.fetch_add(1, Ordering::SeqCst);
            report(sent.load(Ordering::SeqCst), total, msg(sent.load(Ordering::SeqCst), files_done.load(Ordering::SeqCst)));
        }
        f.quit();
    }
    report(total, total, msg(total, nfiles));
    Ok(Some(nfiles))
}
