// SPDX-License-Identifier: GPL-3.0-or-later
//! `.nup` packages (https://github.com/a55uka/nup, format 1): one file holding a game's decrypted base, updates and add-ons.
//! Ported from the fefw-recomp launcher's nup.rs (GPL-3.0). It unpacks into the same layout as a dump folder and as the NSP / XCI
//! extraction (rawdump.rs): `<out>/base/{exefs,romfs}`, `<out>/updates/<version>/{exefs,romfs}`, `<out>/dlc/<title id>/romfs`.
//! Only the exefs and romfs subtrees are written. No keys are needed: a .nup is already decrypted.
//!
//! The unpack can be paused and continued: each file is written as `<name>.part` and renamed when complete, finished files are
//! kept, and `.nup_job` records which package the folder holds (another .nup clears it). When everything is written,
//! `extract.json` lists the input like the NSP / XCI extractor does, so `rawdump::is_extracted` works for both.
//! No crate-local imports, so it can be built alone like rawdump.rs.

use std::fs::{self, File};
use std::io::{BufReader, BufWriter, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};

const NO_DIR: u32 = 0xFFFF_FFFF;
const KIND_BASE: u8 = 0;
const KIND_UPDATE: u8 = 1;
const KIND_ADDON: u8 = 2;
const JOB: &str = ".nup_job";

struct Package {
    title_id: u64,
    root_dir: u32,
    exefs_dir: u32,
    romfs_dir: u32,
    version: u32,
    kind: u8,
}

struct Dir {
    name: String,
    first_child: u32,
    child_count: u32,
}

struct FileNode {
    name: String,
    size: u64,
    extent_offset: u64,
    extent_size: u64,
    codec: u8,
}

struct Nup {
    uuid: [u8; 16],
    file_size: u64,
    packages: Vec<Package>,
    dirs: Vec<Dir>,
    files: Vec<FileNode>,
    children: Vec<u32>,
}

/// What a .nup holds, for the launcher's pages (the same shape as rawdump::Found's fields).
pub struct Info {
    /// "base" or "updates/<version>": the part the game will use
    pub dir: String,
    pub version: u64,
    pub build_id: String,
    pub files: u64,
    pub bytes: u64,
    pub zbic_modules: u64,
    pub notes: Vec<String>,
}

pub fn is_nup(p: &str) -> bool {
    p.to_ascii_lowercase().ends_with(".nup")
}

fn u32_at(b: &[u8], o: usize) -> Result<u32, String> {
    b.get(o..o + 4).map(|s| u32::from_le_bytes(s.try_into().unwrap())).ok_or_else(|| "The .nup file is truncated.".to_string())
}
fn u64_at(b: &[u8], o: usize) -> Result<u64, String> {
    b.get(o..o + 8).map(|s| u64::from_le_bytes(s.try_into().unwrap())).ok_or_else(|| "The .nup file is truncated.".to_string())
}

fn parse(f: &mut File) -> Result<Nup, String> {
    let real_size = f.metadata().map_err(|e| e.to_string())?.len();
    let mut sb = [0u8; 0x80];
    f.read_exact(&mut sb).map_err(|_| "This file is too small to be a .nup package.".to_string())?;
    if &sb[0..4] != b"NUP\x01" {
        return Err("This is not a .nup package (wrong header).".into());
    }
    let toc_offset = u64_at(&sb, 0x08)?;
    let toc_size = u64_at(&sb, 0x10)?;
    let file_size = u64_at(&sb, 0x40)?;
    if u32_at(&sb, 0x4C)? != 0 {
        return Err("This .nup uses features this launcher does not know.".into());
    }
    if file_size != real_size || toc_offset.checked_add(toc_size).map_or(true, |e| e > real_size) || toc_size > (1 << 30) {
        return Err("The .nup file is damaged or incomplete (size mismatch). Download or copy it again.".into());
    }
    let mut uuid = [0u8; 16];
    uuid.copy_from_slice(&sb[0x50..0x60]);

    let mut toc = vec![0u8; toc_size as usize];
    f.seek(SeekFrom::Start(toc_offset)).map_err(|e| e.to_string())?;
    f.read_exact(&mut toc).map_err(|e| e.to_string())?;
    if toc.get(0..4) != Some(b"TOC\x01") {
        return Err("The .nup table of contents is invalid.".into());
    }
    let pkg_count = u32_at(&toc, 4)? as usize;
    let dir_count = u64_at(&toc, 8)? as usize;
    let file_count = u64_at(&toc, 0x10)? as usize;
    let pool_off = u64_at(&toc, 0x18)? as usize;
    let pool_size = u64_at(&toc, 0x20)? as usize;
    let pool = toc.get(pool_off..pool_off.saturating_add(pool_size)).ok_or("The .nup string table is out of bounds.")?;
    if pkg_count > 4096 || dir_count > toc.len() || file_count > toc.len() {
        return Err("The .nup table of contents is invalid.".into());
    }

    let name_at = |off: u32, len: u32| -> Result<String, String> {
        let (o, l) = (off as usize, len as usize);
        let raw = pool.get(o..o.saturating_add(l)).ok_or("A name in the .nup is out of bounds.")?;
        Ok(String::from_utf8_lossy(raw).into_owned())
    };

    let mut pos = 0x30usize;
    let mut packages = Vec::with_capacity(pkg_count);
    for _ in 0..pkg_count {
        packages.push(Package {
            title_id: u64_at(&toc, pos)?,
            root_dir: u32_at(&toc, pos + 0x08)?,
            exefs_dir: u32_at(&toc, pos + 0x0C)?,
            romfs_dir: u32_at(&toc, pos + 0x10)?,
            version: u32_at(&toc, pos + 0x14)?,
            kind: *toc.get(pos + 0x18).ok_or("The .nup file is truncated.")?,
        });
        pos += 0x40;
    }
    let mut dirs = Vec::with_capacity(dir_count);
    for _ in 0..dir_count {
        dirs.push(Dir { name: name_at(u32_at(&toc, pos)?, u32_at(&toc, pos + 4)?)?, first_child: u32_at(&toc, pos + 8)?, child_count: u32_at(&toc, pos + 0xC)? });
        pos += 0x18;
    }
    let mut files = Vec::with_capacity(file_count);
    for _ in 0..file_count {
        files.push(FileNode {
            name: name_at(u32_at(&toc, pos)?, u32_at(&toc, pos + 4)?)?,
            size: u64_at(&toc, pos + 8)?,
            extent_offset: u64_at(&toc, pos + 0x10)?,
            extent_size: u64_at(&toc, pos + 0x18)?,
            codec: *toc.get(pos + 0x40).ok_or("The .nup file is truncated.")?,
        });
        pos += 0x50;
    }
    if pool_off < pos || (pool_off - pos) % 4 != 0 {
        return Err("The .nup table of contents is invalid.".into());
    }
    let children = (0..(pool_off - pos) / 4).map(|i| u32_at(&toc, pos + i * 4)).collect::<Result<Vec<_>, _>>()?;
    Ok(Nup { uuid, file_size, packages, dirs, files, children })
}

/// Names in a package are labels, never paths.
fn safe_name(n: &str) -> Result<&str, String> {
    if n.is_empty() || n == "." || n == ".." || n.contains(['/', '\\', ':', '\0']) {
        return Err(format!("The .nup contains an unsafe file name: {:?}", n));
    }
    Ok(n)
}

impl Nup {
    /// (sub folders, files) of a folder.
    fn entries(&self, d: u32) -> Result<(Vec<u32>, Vec<&FileNode>), String> {
        let dir = self.dirs.get(d as usize).ok_or("The .nup refers to a folder that does not exist.")?;
        let dc = self.dirs.len() as u32;
        let (mut subs, mut files) = (Vec::new(), Vec::new());
        for slot in dir.first_child..dir.first_child.saturating_add(dir.child_count) {
            let id = *self.children.get(slot as usize).ok_or("The .nup folder listing is out of bounds.")?;
            if id < dc {
                subs.push(id);
            } else {
                files.push(self.files.get((id - dc) as usize).ok_or("The .nup refers to a file that does not exist.")?);
            }
        }
        Ok((subs, files))
    }

    /// (file count, bytes) under a folder.
    fn totals(&self, d: u32, depth: u32) -> Result<(u64, u64), String> {
        if depth > 64 {
            return Err("The .nup folder tree is too deep.".into());
        }
        let (subs, files) = self.entries(d)?;
        let mut t = (files.len() as u64, files.iter().map(|f| f.size).sum::<u64>());
        for s in subs {
            let (n, b) = self.totals(s, depth + 1)?;
            t = (t.0 + n, t.1 + b);
        }
        Ok(t)
    }

    /// The program's package: the newest update, else the base.
    fn program(&self) -> Option<&Package> {
        self.packages.iter().filter(|p| p.kind == KIND_UPDATE).max_by_key(|p| p.version).or_else(|| self.packages.iter().find(|p| p.kind == KIND_BASE))
    }

    /// (package, destination below `out`) for everything the unpack writes.
    fn jobs(&self) -> Vec<(&Package, PathBuf)> {
        self.packages
            .iter()
            .filter_map(|p| match p.kind {
                KIND_BASE => Some((p, PathBuf::from("base"))),
                KIND_UPDATE => Some((p, PathBuf::from("updates").join(p.version.to_string()))),
                KIND_ADDON => Some((p, PathBuf::from("dlc").join(format!("{:016X}", p.title_id)))),
                _ => None,
            })
            .collect()
    }
}

/// Reads the first bytes of a file in the package.
fn head(src: &mut File, nup: &Nup, f: &FileNode, n: usize) -> Result<Vec<u8>, String> {
    if f.extent_offset.checked_add(f.extent_size).map_or(true, |e| e > nup.file_size) {
        return Err(format!("{} points outside the .nup file.", f.name));
    }
    src.seek(SeekFrom::Start(f.extent_offset)).map_err(|e| e.to_string())?;
    let part = (&mut *src).take(f.extent_size);
    let mut out = Vec::new();
    match f.codec {
        0 => part.take(n as u64).read_to_end(&mut out),
        1 => zstd::stream::read::Decoder::new(part).map_err(|e| e.to_string())?.take(n as u64).read_to_end(&mut out),
        c => return Err(format!("{} uses an unknown compression ({}).", f.name, c)),
    }
    .map_err(|e| e.to_string())?;
    Ok(out)
}

/// Opens the package and says what it holds. Writes nothing. `title_id` is the game's program id (game.toml).
pub fn info(nup_path: &Path, title_id: &str, main_file: &str) -> Result<Info, String> {
    let mut file = File::open(nup_path).map_err(|e| format!("cannot open {}: {}", nup_path.display(), e))?;
    let nup = parse(&mut file)?;
    let want = u64::from_str_radix(title_id.trim(), 16).map_err(|_| "The game description (game.toml) has no valid [game] title_id.".to_string())?;
    let prog = nup.program().ok_or("This .nup has no base game or update in it.")?;
    // an update's title id is the program id with 0x800 added
    if prog.title_id & !0xFFF != want & !0xFFF {
        return Err(format!("This .nup holds another game (title {:016X}); this launcher needs {:016X}.", prog.title_id, want));
    }
    if prog.exefs_dir == NO_DIR || prog.romfs_dir == NO_DIR {
        return Err("The game program in this .nup has no exefs or romfs.".into());
    }
    let mut notes = Vec::new();
    if prog.kind == KIND_UPDATE && !nup.packages.iter().any(|p| p.kind == KIND_BASE) {
        notes.push("This .nup has the update without the base game. If the game files are incomplete, use a .nup with both.".into());
    }
    let (_, exefs_files) = nup.entries(prog.exefs_dir)?;
    let main = exefs_files.iter().find(|f| f.name == main_file).ok_or(format!("The game program (exefs/{}) is missing from this .nup.", main_file))?;
    // NSO header: flags at 0x0C (bit 7: ZBIC), build id at 0x40
    let h = head(&mut file, &nup, main, 0x60)?;
    if h.len() < 0x60 || &h[..4] != b"NSO0" {
        return Err(format!("exefs/{} in this .nup is not a game program (NSO).", main_file));
    }
    let build_id: String = h[0x40..0x60].iter().map(|b| format!("{:02X}", b)).collect();
    let mut zbic = 0u64;
    for f in &exefs_files {
        if let Ok(h) = head(&mut file, &nup, f, 16) {
            if h.len() == 16 && &h[..4] == b"NSO0" && h[12] & 0x80 != 0 {
                zbic += 1;
            }
        }
    }
    let (files, bytes) = nup.totals(prog.romfs_dir, 0)?;
    Ok(Info {
        dir: if prog.kind == KIND_UPDATE { format!("updates/{}", prog.version) } else { "base".into() },
        version: prog.version as u64,
        build_id,
        files,
        bytes,
        zbic_modules: zbic,
        notes,
    })
}

struct Extractor<'a> {
    nup: &'a Nup,
    src: BufReader<File>,
    done: u64,
    total: u64,
    progress: &'a mut dyn FnMut(u64, u64, &str),
    cancel: &'a dyn Fn() -> bool,
}

impl Extractor<'_> {
    /// Ok(false) = cancelled.
    fn dir(&mut self, d: u32, out: &Path, depth: u32) -> Result<bool, String> {
        if depth > 64 {
            return Err("The .nup folder tree is too deep.".into());
        }
        fs::create_dir_all(out).map_err(|e| format!("cannot create {}: {}", out.display(), e))?;
        let nup = self.nup;
        let (subs, files) = nup.entries(d)?;
        for f in files {
            if !self.file(f, &out.join(safe_name(&f.name)?))? {
                return Ok(false);
            }
        }
        for s in subs {
            if !self.dir(s, &out.join(safe_name(&nup.dirs[s as usize].name)?), depth + 1)? {
                return Ok(false);
            }
        }
        Ok(true)
    }

    fn file(&mut self, f: &FileNode, dest: &Path) -> Result<bool, String> {
        // written by an earlier run (a finished file is renamed from .part only when complete)
        if fs::metadata(dest).map(|m| m.is_file() && m.len() == f.size).unwrap_or(false) {
            self.done += f.size;
            return Ok(true);
        }
        if (self.cancel)() {
            return Ok(false);
        }
        (self.progress)(self.done, self.total, &f.name);
        let part_path = dest.with_file_name(format!("{}.part", f.name));
        let mut out = BufWriter::with_capacity(4 << 20, File::create(&part_path).map_err(|e| format!("cannot write {}: {}", part_path.display(), e))?);
        if f.size > 0 {
            if f.extent_offset.checked_add(f.extent_size).map_or(true, |e| e > self.nup.file_size) {
                return Err(format!("{} points outside the .nup file.", f.name));
            }
            self.src.seek(SeekFrom::Start(f.extent_offset)).map_err(|e| e.to_string())?;
            let mut part = (&mut self.src).take(f.extent_size);
            // copied in pieces so big files show progress and Pause answers within a second or so
            let (base, total, name) = (self.done, self.total, f.name.clone());
            let mut written = 0u64;
            let mut buf = vec![0u8; 4 << 20];
            let mut copy = |r: &mut dyn Read, progress: &mut dyn FnMut(u64, u64, &str), cancel: &dyn Fn() -> bool| -> Result<bool, String> {
                loop {
                    let n = r.read(&mut buf).map_err(|e| format!("{} could not be read: {}", name, e))?;
                    if n == 0 {
                        return Ok(true);
                    }
                    out.write_all(&buf[..n]).map_err(|e| format!("cannot write {}: {}", part_path.display(), e))?;
                    written += n as u64;
                    if written % (64 << 20) < n as u64 {
                        progress(base + written, total, &name);
                        if cancel() {
                            return Ok(false);
                        }
                    }
                }
            };
            let finished = match f.codec {
                0 => copy(&mut part, &mut *self.progress, self.cancel)?,
                1 => {
                    let mut dec = zstd::stream::read::Decoder::new(&mut part).map_err(|e| e.to_string())?;
                    copy(&mut dec, &mut *self.progress, self.cancel)?
                }
                c => return Err(format!("{} uses an unknown compression ({}).", f.name, c)),
            };
            if !finished {
                drop(out);
                let _ = fs::remove_file(&part_path);
                return Ok(false);
            }
            if written != f.size {
                return Err(format!("{} has the wrong size after unpacking: the .nup is damaged.", f.name));
            }
        }
        out.flush().map_err(|e| format!("cannot write {}: {}", part_path.display(), e))?;
        drop(out);
        fs::rename(&part_path, dest).map_err(|e| format!("cannot write {}: {}", dest.display(), e))?;
        self.done += f.size;
        Ok(true)
    }
}

fn uuid_hex(u: &[u8; 16]) -> String {
    u.iter().map(|b| format!("{:02x}", b)).collect()
}

/// Unpacks `nup_path` into `out`. `progress(done bytes, total bytes, current file)`; `cancel` is polled while copying.
/// Ok(true) = done, Ok(false) = paused (run again to continue).
pub fn extract(nup_path: &Path, out: &Path, progress: &mut dyn FnMut(u64, u64, &str), cancel: &dyn Fn() -> bool) -> Result<bool, String> {
    let mut file = File::open(nup_path).map_err(|e| format!("cannot open {}: {}", nup_path.display(), e))?;
    let nup = parse(&mut file)?;
    let stamp = format!("{}:{}", uuid_hex(&nup.uuid), nup.file_size);
    // another package (or an NSP / XCI extraction) in the folder: start clean
    if out.exists() && fs::read_to_string(out.join(JOB)).map(|s| s.trim() != stamp).unwrap_or(true) {
        fs::remove_dir_all(out).map_err(|e| format!("cannot clear {}: {}", out.display(), e))?;
    }
    fs::create_dir_all(out).map_err(|e| format!("cannot create {}: {}", out.display(), e))?;
    let _ = fs::remove_file(out.join("extract.json"));
    fs::write(out.join(JOB), &stamp).map_err(|e| e.to_string())?;

    let jobs = nup.jobs();
    if !jobs.iter().any(|(p, _)| p.kind != KIND_ADDON) {
        return Err("This .nup has no base game or update in it.".into());
    }
    let mut total = 0u64;
    for (p, _) in &jobs {
        for d in [p.exefs_dir, p.romfs_dir] {
            if d != NO_DIR {
                total += nup.totals(d, 0)?.1;
            }
        }
    }
    let mut ex = Extractor { nup: &nup, src: BufReader::with_capacity(1 << 20, file), done: 0, total, progress, cancel };
    for (p, rel) in &jobs {
        if p.root_dir as usize >= nup.dirs.len() {
            return Err("The .nup package tree is invalid.".into());
        }
        for (d, name) in [(p.exefs_dir, "exefs"), (p.romfs_dir, "romfs")] {
            if d != NO_DIR && !ex.dir(d, &out.join(rel).join(name), 0)? {
                return Ok(false);
            }
        }
    }
    (ex.progress)(ex.total, ex.total, "");
    let size = fs::metadata(nup_path).map(|m| m.len()).unwrap_or(0);
    let path = nup_path.to_string_lossy().replace('\\', "/");
    let record = format!("{{\"inputs\":[{{\"path\":{},\"size\":{}}}],\"nup\":\"{}\"}}\n", json_str(&path), size, stamp);
    fs::write(out.join("extract.json"), record).map_err(|e| e.to_string())?;
    Ok(true)
}

fn json_str(s: &str) -> String {
    let mut o = String::from("\"");
    for c in s.chars() {
        match c {
            '"' => o.push_str("\\\""),
            '\\' => o.push_str("\\\\"),
            c if (c as u32) < 0x20 => o.push_str(&format!("\\u{:04x}", c as u32)),
            c => o.push(c),
        }
    }
    o.push('"');
    o
}
