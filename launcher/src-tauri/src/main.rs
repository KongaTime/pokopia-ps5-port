// SPDX-License-Identifier: GPL-3.0-or-later
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod dlss;
mod dump;
mod ftp;
mod gamebuild;
mod ini;
mod manifest;
mod nup;
mod play;
mod ps5;
mod rawdump;
mod settings;
mod shaders;

use serde::{Deserialize, Serialize};
use std::fs;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use tauri::Emitter;
use tauri::{AppHandle, Manager, State};
use tauri_plugin_dialog::DialogExt;

/// The launcher's own options, stored in the user's app config folder.
#[derive(Serialize, Deserialize, Clone, Default)]
#[serde(default)]
struct Config {
    /// Root of the decrypted dump (the folder with base/ and updates/).
    dump_dir: String,
    /// Folder with the game exe. Empty: the folder the launcher is in.
    game_dir: String,
    minimize_on_play: bool,
    /// Raw dump mode: the player's NSP / XCI files, or one `.nup` package. When set, Set up first extracts them into `<game>/dump`
    /// (rawdump.rs, nup.rs), which is then `dump_dir`. Choosing a dump folder clears it.
    raw_inputs: Vec<String>,
    /// prod.keys and optionally title.keys for raw_inputs (paths only; the launcher never reads them).
    key_files: Vec<String>,
    /// PS5 mode: the console's address (its FTP server on port 2121 takes the install).
    ps5_host: String,
}

fn config_path(app: &AppHandle) -> Result<PathBuf, String> {
    // %APPDATA%\<launcher.identifier>: one settings folder per game, whatever app id the build was given
    let id = manifest::get()?.launcher.identifier.clone();
    let dir = app.path().config_dir().map_err(|e| e.to_string())?.join(id);
    fs::create_dir_all(&dir).map_err(|e| format!("cannot create {}: {}", dir.display(), e))?;
    Ok(dir.join("launcher.json"))
}

fn load_config(app: &AppHandle) -> Config {
    config_path(app)
        .ok()
        .and_then(|p| fs::read_to_string(p).ok())
        .and_then(|t| serde_json::from_str(&t).ok())
        .unwrap_or_default()
}

fn exe_dir() -> PathBuf {
    std::env::current_exe().ok().and_then(|p| p.parent().map(|d| d.to_path_buf())).unwrap_or_else(|| PathBuf::from("."))
}

fn game_dir_of(cfg: &Config) -> PathBuf {
    if cfg.game_dir.trim().is_empty() {
        exe_dir()
    } else {
        PathBuf::from(cfg.game_dir.trim())
    }
}

#[derive(Serialize)]
struct Overview {
    game_dir: String,
    default_game_dir: String,
    exe_exists: bool,
}

/// The game manifest as the pages need it.
#[tauri::command]
fn get_game() -> Result<manifest::GameInfo, String> {
    Ok(manifest::get()?.info())
}

#[tauri::command]
fn get_config(app: AppHandle) -> Config {
    load_config(&app)
}

#[tauri::command]
fn save_config(app: AppHandle, config: Config) -> Result<(), String> {
    let text = serde_json::to_string_pretty(&config).map_err(|e| e.to_string())?;
    fs::write(config_path(&app)?, text).map_err(|e| format!("cannot save launcher settings: {}", e))
}

#[tauri::command]
fn get_overview(app: AppHandle) -> Overview {
    let cfg = load_config(&app);
    let dir = game_dir_of(&cfg);
    Overview {
        game_dir: dump::fwd(&dir),
        default_game_dir: dump::fwd(&exe_dir()),
        exe_exists: manifest::get().map_or(false, |m| dir.join(&m.files.exe).is_file()),
    }
}

#[tauri::command]
async fn pick_folder(app: AppHandle, title: String, start: Option<String>) -> Option<String> {
    let mut dlg = app.dialog().file().set_title(title);
    if let Some(s) = start.filter(|s| PathBuf::from(s).is_dir()) {
        dlg = dlg.set_directory(s);
    }
    dlg.blocking_pick_folder().and_then(|p| p.into_path().ok()).map(|p| dump::fwd(&p))
}

#[tauri::command]
fn validate_dump(path: String) -> dump::DumpReport {
    dump::scan(&path)
}

/// Validates, stores the dump folder, and points `[fs] romfs` in the game ini at the chosen romfs.
#[tauri::command]
fn save_dump(app: AppHandle, path: String) -> Result<dump::DumpReport, String> {
    let report = dump::scan(&path);
    let chosen = report.chosen.clone().ok_or_else(|| report.errors.join(" "))?;
    let m = manifest::get()?;
    let mut cfg = load_config(&app);
    cfg.dump_dir = report.path.clone();
    cfg.raw_inputs.clear();
    cfg.key_files.clear();
    save_config(app.clone(), cfg.clone())?;
    settings::apply(
        &game_dir_of(&cfg),
        &m.files.ini,
        &[
            settings::Edit { file: "game".into(), section: "fs".into(), key: "exefs".into(), value: chosen.exefs },
            settings::Edit { file: "game".into(), section: "fs".into(), key: "romfs".into(), value: chosen.romfs },
        ],
    )?;
    Ok(report)
}

#[tauri::command]
async fn pick_files(app: AppHandle, title: String, filter_name: String, extensions: Vec<String>, multiple: bool) -> Vec<String> {
    let exts: Vec<&str> = extensions.iter().map(|e| e.as_str()).collect();
    let dlg = app.dialog().file().set_title(title).add_filter(filter_name, &exts);
    let picked = if multiple { dlg.blocking_pick_files().unwrap_or_default() } else { dlg.blocking_pick_file().into_iter().collect() };
    picked.into_iter().filter_map(|p| p.into_path().ok()).map(|p| dump::fwd(&p)).collect()
}

/// True when the raw dump is one `.nup` package (nup.rs: already decrypted, no keys). Mixing it with NSP / XCI files is refused.
fn nup_mode(inputs: &[String]) -> Result<bool, String> {
    let n = inputs.iter().filter(|p| nup::is_nup(p)).count();
    if n == 0 {
        return Ok(false);
    }
    if inputs.len() > 1 {
        return Err("Pick only the .nup file: it already holds the base game and the update. Remove the other dump files.".into());
    }
    if !std::path::Path::new(&inputs[0]).is_file() {
        return Err(format!("{} does not exist or cannot be read.", inputs[0]));
    }
    Ok(true)
}

/// Raw dump mode: what the dump files hold, read with the player's keys, or from the .nup (nothing is written).
#[tauri::command]
async fn check_raw(inputs: Vec<String>, keys: Vec<String>) -> Result<rawdump::Found, String> {
    let m = manifest::get()?;
    let title_id = m.game.title_id.clone();
    if nup_mode(&inputs)? {
        let main = m.main_file();
        let path = PathBuf::from(&inputs[0]);
        let i = tauri::async_runtime::spawn_blocking(move || nup::info(&path, &title_id, &main)).await.map_err(|e| e.to_string())??;
        return Ok(rawdump::Found { dir: i.dir, version: i.version, display_version: String::new(), build_id: i.build_id, files: i.files, bytes: i.bytes, zbic_modules: i.zbic_modules, notes: i.notes });
    }
    let tool = rawdump::find_tool(&exe_dir().join("tools"))?;
    tauri::async_runtime::spawn_blocking(move || rawdump::check(&tool, &inputs, &keys, &title_id)).await.map_err(|e| e.to_string())?
}

/// Raw dump mode, run by Set up and by the PS5 install: extracts the NSP / XCI files or unpacks the .nup into `out`, unless that
/// was done already. `report(done, total, message)`. Ok(true) = ready, Ok(false) = paused.
fn extract_raw(cfg: &Config, tool: Option<&std::path::Path>, title_id: &str, out: &std::path::Path, cancel: &AtomicBool, mut report: impl FnMut(u64, u64, String)) -> Result<bool, String> {
    if rawdump::is_extracted(out, &cfg.raw_inputs) {
        return Ok(true);
    }
    if nup_mode(&cfg.raw_inputs)? {
        return nup::extract(
            std::path::Path::new(&cfg.raw_inputs[0]),
            out,
            &mut |d, t, _| report(d, t, "Unpacking the game files from your .nup...".to_string()),
            &|| cancel.load(Ordering::SeqCst),
        );
    }
    let tool = tool.ok_or("The dump extractor (tools/extract/nx_extract.exe) is missing.")?;
    rawdump::extract(tool, &cfg.raw_inputs, &cfg.key_files, title_id, out, cancel, |t| {
        let message = match t.phase.as_str() {
            "verify" => "Checking your dump files...".to_string(),
            "romfs" => "Extracting the game data from your dump...".to_string(),
            _ => t.message,
        };
        report(t.done, t.total, message)
    })
}

/// Raw dump mode: checks and stores the dump files and keys; Set up extracts them into `<game>/dump` and uses that folder.
#[tauri::command]
async fn save_raw(app: AppHandle, inputs: Vec<String>, keys: Vec<String>) -> Result<rawdump::Found, String> {
    let found = check_raw(inputs.clone(), keys.clone()).await?;
    let mut cfg = load_config(&app);
    cfg.raw_inputs = inputs;
    cfg.key_files = keys;
    cfg.dump_dir = dump::fwd(&game_dir_of(&cfg).join(rawdump::OUT_REL));
    save_config(app, cfg)?;
    Ok(found)
}

#[tauri::command]
fn read_settings(app: AppHandle) -> Result<settings::SettingsDump, String> {
    Ok(settings::read(&game_dir_of(&load_config(&app)), &manifest::get()?.files.ini))
}

#[tauri::command]
fn write_settings(app: AppHandle, edits: Vec<settings::Edit>) -> Result<(), String> {
    settings::apply(&game_dir_of(&load_config(&app)), &manifest::get()?.files.ini, &edits)
}

#[tauri::command]
fn play(app: AppHandle) -> Result<(), String> {
    let cfg = load_config(&app);
    if cfg.dump_dir.trim().is_empty() {
        return Err("Choose your game files first (Setup).".into());
    }
    let report = dump::scan(&cfg.dump_dir);
    let chosen = report.chosen.ok_or_else(|| format!("The game files at {} are not usable: {}", cfg.dump_dir, report.errors.join(" ")))?;
    let m = manifest::get()?;
    let dir = game_dir_of(&cfg);
    if gamebuild::is_package_b(&exe_dir().join("tools")) && !gamebuild::is_built(&dir, &m.files.exe) {
        return Err("Build the game first (Game files, Set up).".into());
    }
    // the shader cache may be incomplete: the runtime translates missing shaders while the game runs
    settings::ensure_play_paths(&dir, &m.files.ini, &chosen.exefs, &chosen.romfs)?;
    play::start(&app, &dir, cfg.minimize_on_play)
}

/// Shader setup: the running job's progress and its cancel flag.
#[derive(Default)]
struct SetupState {
    progress: Mutex<shaders::Progress>,
    cancel: AtomicBool,
}

#[derive(Serialize)]
struct SetupStatus {
    progress: shaders::Progress,
    /// setup.json says finished and the cache is there
    complete: bool,
    /// an earlier setup started (Set up continues it)
    started: bool,
    /// the game exe is built: Play works, the shader cache may still be filling
    playable: bool,
    shaders_done: u64,
    shaders_total: u64,
}

#[tauri::command]
fn setup_status(app: AppHandle, state: State<Arc<SetupState>>) -> Result<SetupStatus, String> {
    let m = manifest::get()?;
    let dir = game_dir_of(&load_config(&app));
    let mut progress = state.progress.lock().unwrap().clone();
    if progress.state.is_empty() {
        progress.state = "idle".into();
    }
    let built = !gamebuild::is_package_b(&exe_dir().join("tools")) || gamebuild::is_built(&dir, &m.files.exe);
    let started = !shaders::read_marker(&dir).romfs.is_empty() || dir.join("build").join("state.json").is_file();
    let (shaders_done, shaders_total) = shaders::cache_progress(&dir);
    Ok(SetupStatus { progress, complete: built && shaders::is_complete(&dir), started, playable: built && dir.join(&m.files.exe).is_file(), shaders_done, shaders_total })
}

/// First-run setup: (0) in raw dump mode, the NSP / XCI files extracted into `<game>/dump` (rawdump.rs), (a) the game ini paths,
/// (b) in a model (b) release, the game exe built from the player's dump (gamebuild.rs; ZBIC modules converted to plain NSOs first),
/// (c) the shader cache in shader_cache/spirv, on a worker thread with "setup-status" events.
#[tauri::command]
fn start_setup(app: AppHandle, state: State<Arc<SetupState>>) -> Result<(), String> {
    let m = manifest::get()?;
    let cfg = load_config(&app);
    let raw = !cfg.raw_inputs.is_empty();
    if !raw {
        dump::scan(&cfg.dump_dir).chosen.ok_or_else(|| "Choose usable game files first.".to_string())?;
    }
    let dir = game_dir_of(&cfg);
    let tools_dir = exe_dir().join("tools");
    let package_b = gamebuild::is_package_b(&tools_dir);
    if !package_b && !dir.join(&m.files.exe).is_file() {
        return Err(format!("{0} was not found in {1}. Put the launcher next to {0} or set the game folder in Settings.", m.files.exe, dump::fwd(&dir)));
    }
    let extract_tool = if raw && !nup_mode(&cfg.raw_inputs)? { Some(rawdump::find_tool(&tools_dir)?) } else { rawdump::find_tool(&tools_dir).ok() };
    {
        let mut p = state.progress.lock().unwrap();
        if p.state == "running" {
            return Err("Setup is already running.".into());
        }
        *p = shaders::Progress { state: "running".into(), phase: "check".into(), message: "Checking the setup tools...".into(), ..Default::default() };
    }
    let st = state.inner().clone();
    st.cancel.store(false, Ordering::SeqCst);
    let jobs = std::thread::available_parallelism().map(|n| n.get().saturating_sub(1)).unwrap_or(2).max(1);
    std::thread::spawn(move || {
        let emit = |p: shaders::Progress| {
            *st.progress.lock().unwrap() = p.clone();
            let _ = app.emit("setup-status", p);
        };
        let result = (|| -> Result<bool, String> {
            if raw {
                let out = dir.join(rawdump::OUT_REL);
                let done = extract_raw(&cfg, extract_tool.as_deref(), &m.game.title_id, &out, &st.cancel, |done, total, message| {
                    emit(shaders::Progress { state: "running".into(), phase: "dump".into(), done, total, message })
                })?;
                if !done {
                    return Ok(false);
                }
            }
            let report = dump::scan(&cfg.dump_dir);
            let chosen = report.chosen.ok_or_else(|| format!("The game files at {} are not usable: {}", cfg.dump_dir, report.errors.join(" ")))?;
            settings::ensure_play_paths(&dir, &m.files.ini, &chosen.exefs, &chosen.romfs)?;
            // the lifter and the shader extractor read plain NSOs; the runtime reads ZBIC itself, so the ini keeps the dump's ExeFS
            let exefs = rawdump::exefs_for_build(extract_tool.as_deref(), &PathBuf::from(&chosen.exefs), &dir)?;
            if package_b && !gamebuild::is_built(&dir, &m.files.exe) {
                let tools = gamebuild::find_tools(&tools_dir, &m.runtime_lib())?;
                let build_job = gamebuild::Job { game_dir: dir.clone(), exefs: exefs.clone(), jobs: jobs + 1 };
                let built = gamebuild::run(&build_job, &tools, &st.cancel, |t| {
                    emit(shaders::Progress { state: "running".into(), phase: t.phase, done: t.done, total: t.total, message: t.message })
                })?;
                if !built {
                    return Ok(false);
                }
            }
            let tools = shaders::find_tools(&tools_dir)?;
            let job = shaders::Job { game_dir: dir.clone(), romfs: PathBuf::from(&chosen.romfs), main: exefs.join(m.main_file()), jobs: 0, limit: 0 };
            shaders::run(&job, &tools, &st.cancel, |p| emit(p))
        })();
        let end = match result {
            Ok(true) => shaders::Progress { state: "done".into(), phase: "finish".into(), message: "Setup finished.".into(), ..Default::default() },
            Ok(false) => shaders::Progress { state: "cancelled".into(), message: "Setup paused. Set up again continues where it stopped.".into(), ..Default::default() },
            Err(e) => shaders::Progress { state: "error".into(), message: e, ..Default::default() },
        };
        emit(end);
    });
    Ok(())
}

#[tauri::command]
fn cancel_setup(state: State<Arc<SetupState>>) {
    state.cancel.store(true, Ordering::SeqCst);
}

/// PS5 mode: the running install's status and its cancel flag.
#[derive(Default)]
struct Ps5State {
    status: Mutex<ps5::Status>,
    cancel: AtomicBool,
}

#[derive(Serialize)]
struct Ps5Overview {
    /// the package has the PS5 toolchain (tools/ps5)
    available: bool,
    host: String,
    status: ps5::Status,
}

#[tauri::command]
fn ps5_status(app: AppHandle, state: State<Arc<Ps5State>>) -> Ps5Overview {
    let cfg = load_config(&app);
    let mut status = state.status.lock().unwrap().clone();
    if !status.running {
        // between runs: what is done, from the files on this PC (the last run's outcome and messages stay)
        let snap = ps5::snapshot(&game_dir_of(&cfg), &cfg.ps5_host);
        if status.steps.len() == snap.len() {
            for (s, n) in status.steps.iter_mut().zip(snap) {
                if n.state == "done" || s.state == "running" {
                    *s = n;
                }
            }
        } else {
            status.steps = snap;
        }
    }
    Ps5Overview { available: ps5::available(&exe_dir().join("tools")), host: cfg.ps5_host, status }
}

/// Looks for PS5s with their FTP server running on this PC's network (ps5::discover).
#[tauri::command]
async fn ps5_find() -> Result<Vec<ps5::Found>, String> {
    tauri::async_runtime::spawn_blocking(ps5::discover).await.map_err(|e| e.to_string())?
}

/// Stores the console address and checks that its FTP server answers.
#[tauri::command]
async fn ps5_check(app: AppHandle, host: String) -> Result<String, String> {
    let host = ps5::clean_host(&host)?;
    let mut cfg = load_config(&app);
    cfg.ps5_host = host.clone();
    save_config(app, cfg)?;
    tauri::async_runtime::spawn_blocking(move || ps5::check_console(&host)).await.map_err(|e| e.to_string())?
}

/// PS5 mode: build, make the shader cache, upload (ps5.rs), on a worker thread with "ps5-status" events. Each step that is done is
/// skipped, so after Pause this continues.
#[tauri::command]
fn ps5_start(app: AppHandle, host: String, state: State<Arc<Ps5State>>, setup: State<Arc<SetupState>>) -> Result<(), String> {
    let host = ps5::clean_host(&host)?;
    let mut cfg = load_config(&app);
    if cfg.ps5_host != host {
        cfg.ps5_host = host.clone();
        save_config(app.clone(), cfg.clone())?;
    }
    if setup.progress.lock().unwrap().state == "running" {
        return Err("The PC setup is running. Pause it first (Game files).".into());
    }
    let raw = !cfg.raw_inputs.is_empty();
    if raw {
        nup_mode(&cfg.raw_inputs)?;
    } else {
        dump::scan(&cfg.dump_dir).chosen.ok_or_else(|| "Choose your game files first.".to_string())?;
    }
    let m = manifest::get()?;
    let tools_dir = exe_dir().join("tools");
    if !ps5::available(&tools_dir) {
        return Err("This download has no PS5 toolchain (tools/ps5). Use a release that includes PS5 mode.".into());
    }
    {
        let mut s = state.status.lock().unwrap();
        if s.running {
            return Err("The PS5 install is already running.".into());
        }
        s.running = true;
    }
    let st = state.inner().clone();
    st.cancel.store(false, Ordering::SeqCst);
    let dir = game_dir_of(&cfg);
    let jobs = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(2).max(1);
    let extract_tool = rawdump::find_tool(&tools_dir).ok();
    std::thread::spawn(move || {
        let emit = |s: &ps5::Status| {
            *st.status.lock().unwrap() = s.clone();
            let _ = app.emit("ps5-status", s.clone());
        };
        let result = (|| -> Result<bool, String> {
            if raw {
                // the .nup or the NSP / XCI files first become a dump folder in <game>/dump, shown on the first step's bar
                let mut s = ps5::Status { running: true, steps: ps5::snapshot(&dir, &host), ..Default::default() };
                let done = extract_raw(&cfg, extract_tool.as_deref(), &m.game.title_id, &dir.join(rawdump::OUT_REL), &st.cancel, |done, total, message| {
                    if let Some(step) = s.steps.first_mut() {
                        step.state = "running".into();
                        step.done = done;
                        step.total = total;
                        step.message = message;
                    }
                    emit(&s);
                })?;
                if !done {
                    return Ok(false);
                }
            }
            let report = dump::scan(&cfg.dump_dir);
            let chosen = report.chosen.ok_or_else(|| format!("The game files at {} are not usable: {}", cfg.dump_dir, report.errors.join(" ")))?;
            // the lifter and the shader extractor read plain NSOs (rawdump.rs); the console gets the dump's own exefs
            let exefs = PathBuf::from(&chosen.exefs);
            let plain = rawdump::exefs_for_build(extract_tool.as_deref(), &exefs, &dir)?;
            let job = ps5::Job {
                game_dir: dir.clone(),
                tools_dir: tools_dir.clone(),
                exefs: plain.clone(),
                dump_exefs: exefs,
                romfs: PathBuf::from(&chosen.romfs),
                main: plain.join(m.main_file()),
                host: host.clone(),
                jobs,
            };
            ps5::run(&job, &st.cancel, |s| emit(s))
        })();
        let mut s = st.status.lock().unwrap().clone();
        s.running = false;
        match result {
            Ok(true) => {
                s.outcome = "done".into();
                s.message = format!("Done. Start {} on your PS5.", ps5::TITLE_ID);
            }
            Ok(false) => {
                s.outcome = "paused".into();
                s.message = "Paused. Continue picks up where it stopped.".into();
            }
            Err(e) => {
                for step in s.steps.iter_mut().filter(|x| x.state == "running") {
                    step.state = "error".into();
                }
                s.outcome = "error".into();
                s.message = e;
            }
        }
        emit(&s);
    });
    Ok(())
}

#[tauri::command]
fn ps5_cancel(state: State<Arc<Ps5State>>) {
    state.cancel.store(true, Ordering::SeqCst);
}

#[tauri::command]
fn dlss_status(app: AppHandle) -> dlss::Status {
    dlss::status(&game_dir_of(&load_config(&app)), &exe_dir().join("tools"))
}

/// Downloads NVIDIA's nvngx_dlss.dll (see dlss.rs) on a worker thread, with "dlss-progress" events.
#[tauri::command]
async fn fetch_dlss(app: AppHandle) -> Result<(), String> {
    let dir = game_dir_of(&load_config(&app));
    let tools_dir = exe_dir().join("tools");
    tauri::async_runtime::spawn_blocking(move || {
        dlss::fetch(&dir, &tools_dir, |p| {
            let _ = app.emit("dlss-progress", p);
        })
    })
    .await
    .map_err(|e| e.to_string())?
}

#[tauri::command]
fn game_status(state: State<play::PlayState>) -> play::Status {
    state.0.lock().unwrap().clone()
}

#[tauri::command]
fn open_game_file(app: AppHandle, name: String) -> Result<(), String> {
    if !manifest::get()?.report_files().contains(&name) {
        return Err("That file cannot be opened from here.".into());
    }
    let path = game_dir_of(&load_config(&app)).join(&name);
    if !path.is_file() {
        return Err(format!("{} does not exist.", name));
    }
    play::open_with_shell(&path)
}

#[tauri::command]
fn open_game_dir(app: AppHandle) -> Result<(), String> {
    play::open_with_shell(&game_dir_of(&load_config(&app)))
}


/// Test runs only (<ID>_LAUNCHER_EVAL_FILE set, the id of the manifest uppercased): the page writes a result next to the eval file, <file>.out.
#[tauri::command]
fn test_report(text: String) -> Result<(), String> {
    let path = std::env::var(manifest::get()?.eval_env_name()).map_err(|_| "not a test run".to_string())?;
    std::fs::write(format!("{path}.out"), text).map_err(|e| e.to_string())
}

fn main() {
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(play::PlayState::default())
        .manage(Arc::new(SetupState::default()))
        .manage(Arc::new(Ps5State::default()))
        .setup(|app| {
            // The window carries the game's launcher title from the manifest (tauri.conf.json has a neutral one).
            if let (Ok(m), Some(win)) = (manifest::get(), app.get_webview_window("main")) {
                let _ = win.set_title(&m.launcher.window_title);
            }
            // Test runs drive the page without input: <ID>_LAUNCHER_EVAL_FILE=<path>. A JS file written there is run in the page once and
            // deleted; it reports back through document.title (the window title). WebView2's DevTools port does not open for Tauri apps here.
            if let Some(path) = manifest::get().ok().and_then(|m| std::env::var(m.eval_env_name()).ok()) {
                if let Some(win) = app.get_webview_window("main") {
                    std::thread::spawn(move || loop {
                        std::thread::sleep(std::time::Duration::from_millis(300));
                        if let Ok(js) = std::fs::read_to_string(&path) {
                            let _ = std::fs::remove_file(&path);
                            let _ = win.eval(&js);
                        }
                    });
                }
            }
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            test_report,
            get_game,
            get_config,
            save_config,
            get_overview,
            pick_folder,
            pick_files,
            check_raw,
            save_raw,
            validate_dump,
            save_dump,
            read_settings,
            write_settings,
            play,
            setup_status,
            start_setup,
            cancel_setup,
            dlss_status,
            fetch_dlss,
            game_status,
            open_game_file,
            open_game_dir,
            ps5_status,
            ps5_check,
            ps5_find,
            ps5_start,
            ps5_cancel,
        ])
        .run(tauri::generate_context!())
        .expect("error while running the launcher");
}
