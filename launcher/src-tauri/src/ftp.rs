// SPDX-License-Identifier: GPL-3.0-or-later
//! A small FTP client for the PS5 install (ps5.rs): the console's homebrew FTP server (port 2121, anonymous) takes the game data,
//! the shader cache and the title. Passive mode, binary type; the commands the server is known to have: USER/PASS, TYPE, PASV,
//! MKD, LIST (it has no NLST), SIZE, STOR, DELE. The data connection always goes to the control connection's address (the server
//! may announce another one). No crate-local imports, so examples can build this file alone.

use std::io::{BufRead, BufReader, Read, Write};
use std::net::{Shutdown, SocketAddr, TcpStream, ToSocketAddrs};
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

pub struct Ftp {
    ctrl: BufReader<TcpStream>,
    peer: SocketAddr,
}

/// One entry of a LIST.
pub struct Entry {
    pub name: String,
    pub size: u64,
    pub dir: bool,
}

const CTRL_TIMEOUT: Duration = Duration::from_secs(60);
const DATA_TIMEOUT: Duration = Duration::from_secs(300);

fn connect(addr: &str, timeout: Duration) -> Result<TcpStream, String> {
    let sa = addr.to_socket_addrs().map_err(|e| format!("{}: {}", addr, e))?.next().ok_or_else(|| format!("{}: no address", addr))?;
    let s = TcpStream::connect_timeout(&sa, timeout).map_err(|e| format!("cannot reach {}: {}", addr, e))?;
    s.set_read_timeout(Some(DATA_TIMEOUT)).ok();
    s.set_write_timeout(Some(DATA_TIMEOUT)).ok();
    Ok(s)
}

impl Ftp {
    pub fn open(host: &str, port: u16) -> Result<Ftp, String> {
        let s = connect(&format!("{}:{}", host, port), Duration::from_secs(8))?;
        s.set_read_timeout(Some(CTRL_TIMEOUT)).ok();
        let peer = s.peer_addr().map_err(|e| e.to_string())?;
        let mut f = Ftp { ctrl: BufReader::new(s), peer };
        f.expect(&[220])?;
        let (code, _) = f.cmd("USER anonymous")?;
        if code == 331 {
            f.cmd_ok("PASS anonymous", &[230, 202])?;
        } else if code != 230 {
            return Err(format!("the FTP server refused the login ({})", code));
        }
        f.cmd_ok("TYPE I", &[200])?;
        Ok(f)
    }

    fn reply(&mut self) -> Result<(u32, String), String> {
        let mut first = String::new();
        if self.ctrl.read_line(&mut first).map_err(|e| format!("FTP: {}", e))? == 0 {
            return Err("FTP: the console closed the connection".into());
        }
        let code: u32 = first.get(0..3).and_then(|c| c.parse().ok()).ok_or_else(|| format!("FTP: unexpected reply {:?}", first.trim()))?;
        let mut text = first.trim_end().to_string();
        if first.as_bytes().get(3) == Some(&b'-') {
            // multi-line reply: until "<code> "
            loop {
                let mut l = String::new();
                if self.ctrl.read_line(&mut l).map_err(|e| format!("FTP: {}", e))? == 0 {
                    break;
                }
                text.push('\n');
                text.push_str(l.trim_end());
                if l.starts_with(&format!("{} ", code)) {
                    break;
                }
            }
        }
        Ok((code, text))
    }

    fn expect(&mut self, ok: &[u32]) -> Result<String, String> {
        let (code, text) = self.reply()?;
        if ok.contains(&code) {
            Ok(text)
        } else {
            Err(format!("FTP: {}", text))
        }
    }

    fn cmd(&mut self, c: &str) -> Result<(u32, String), String> {
        let s = self.ctrl.get_mut();
        s.write_all(format!("{}\r\n", c).as_bytes()).map_err(|e| format!("FTP: {}", e))?;
        self.reply()
    }

    fn cmd_ok(&mut self, c: &str, ok: &[u32]) -> Result<String, String> {
        let (code, text) = self.cmd(c)?;
        if ok.contains(&code) {
            Ok(text)
        } else {
            Err(format!("FTP {}: {}", c.split(' ').next().unwrap_or(c), text))
        }
    }

    fn pasv(&mut self) -> Result<TcpStream, String> {
        let text = self.cmd_ok("PASV", &[227])?;
        let inner = text.split('(').nth(1).and_then(|r| r.split(')').next()).ok_or_else(|| format!("FTP: bad PASV reply {}", text))?;
        let n: Vec<u16> = inner.split(',').filter_map(|x| x.trim().parse().ok()).collect();
        if n.len() != 6 {
            return Err(format!("FTP: bad PASV reply {}", text));
        }
        let port = n[4] * 256 + n[5];
        connect(&format!("{}:{}", self.peer.ip(), port), Duration::from_secs(10))
    }

    pub fn mkd(&mut self, path: &str) {
        let _ = self.cmd(&format!("MKD {}", path));
    }

    /// Makes every folder of `path` (absolute), ignoring "exists".
    pub fn mkdirs(&mut self, path: &str) {
        let mut cur = String::new();
        for part in path.split('/').filter(|p| !p.is_empty()) {
            cur.push('/');
            cur.push_str(part);
            self.mkd(&cur);
        }
    }

    pub fn size(&mut self, path: &str) -> Result<Option<u64>, String> {
        let (code, text) = self.cmd(&format!("SIZE {}", path))?;
        Ok(if code == 213 { text.get(4..).and_then(|s| s.trim().parse().ok()) } else { None })
    }

    pub fn dele(&mut self, path: &str) -> Result<bool, String> {
        Ok(self.cmd(&format!("DELE {}", path))?.0 == 250)
    }

    /// The folder's entries; an empty list when it does not exist.
    pub fn list(&mut self, dir: &str) -> Result<Vec<Entry>, String> {
        let mut data = self.pasv()?;
        let (code, text) = self.cmd(&format!("LIST {}", dir))?;
        if code == 550 || code == 450 {
            return Ok(Vec::new());
        }
        if code != 150 && code != 125 {
            return Err(format!("FTP LIST: {}", text));
        }
        let mut raw = Vec::new();
        data.read_to_end(&mut raw).map_err(|e| format!("FTP LIST: {}", e))?;
        drop(data);
        self.expect(&[226, 250])?;
        let mut out = Vec::new();
        for l in String::from_utf8_lossy(&raw).lines() {
            // unix "ls -l": perms links owner group size month day time-or-year name...
            let mut it = l.splitn(9, char::is_whitespace).filter(|s| !s.is_empty());
            let perms = it.next().unwrap_or("");
            let fields: Vec<&str> = l.split_whitespace().collect();
            if fields.len() < 9 {
                continue;
            }
            // the name is everything after the 8th field (names may hold spaces)
            let mut rest = l;
            for _ in 0..8 {
                rest = rest.trim_start();
                rest = &rest[rest.find(char::is_whitespace).unwrap_or(rest.len())..];
            }
            let name = rest.trim_start().to_string();
            if name == "." || name == ".." || name.is_empty() {
                continue;
            }
            out.push(Entry { name, size: fields[4].parse().unwrap_or(0), dir: perms.starts_with('d') });
        }
        Ok(out)
    }

    /// Uploads `local` to `remote`, reporting each block written. Ok(false) = cancelled (the partial file stays: its size differs,
    /// so the next run sends it again).
    pub fn stor(&mut self, local: &std::path::Path, remote: &str, cancel: &AtomicBool, mut sent: impl FnMut(u64)) -> Result<bool, String> {
        let mut file = std::fs::File::open(local).map_err(|e| format!("cannot read {}: {}", local.display(), e))?;
        let mut data = self.pasv()?;
        let (code, text) = self.cmd(&format!("STOR {}", remote))?;
        if code != 150 && code != 125 {
            return Err(format!("FTP STOR {}: {}", remote, text));
        }
        let mut buf = vec![0u8; 1 << 20];
        loop {
            if cancel.load(Ordering::SeqCst) {
                let _ = data.shutdown(Shutdown::Both);
                drop(data);
                let _ = self.reply();
                return Ok(false);
            }
            let n = file.read(&mut buf).map_err(|e| format!("cannot read {}: {}", local.display(), e))?;
            if n == 0 {
                break;
            }
            data.write_all(&buf[..n]).map_err(|e| format!("FTP upload of {}: {}", remote, e))?;
            sent(n as u64);
        }
        data.flush().ok();
        let _ = data.shutdown(Shutdown::Write);
        drop(data);
        self.expect(&[226, 250])?;
        Ok(true)
    }

    pub fn quit(mut self) {
        let _ = self.cmd("QUIT");
    }
}
