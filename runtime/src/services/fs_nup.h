// SPDX-License-Identifier: GPL-3.0-or-later
// `.nup` packages (https://github.com/a55uka/nup, format 1): one file with a game's decrypted base, updates and add-ons. The runtime reads
// the game program's exefs and romfs straight out of it, so a console needs only the one file (fs_romfs.cpp serves the romfs from it in
// place; extract_files() writes the few exefs files out). Only stored (uncompressed, codec 0) files can be read in place: a package with
// zstd-compressed files has to be unpacked on a PC first. The launcher's reader (launcher/src-tauri/src/nup.rs) is the same format.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nxrt::services::fs::nup {

struct Entry {
    std::string name;            // UTF-8, never a path
    bool dir = false;
    uint64_t size = 0;           // files: bytes once unpacked
    uint64_t offset = 0;         // files: where the stored bytes start in the .nup
    uint64_t stored = 0;         // files: stored bytes
    uint8_t codec = 0;           // files: 0 stored as is, 1 zstd
    std::vector<Entry> kids;     // folders: sorted by name bytes
};

struct Program {
    uint64_t title_id = 0;
    uint32_t version = 0;        // 0 for the base
    bool update = false;         // the newest update's exefs (its romfs is layered over the base's)
    bool has_exefs = false, has_romfs = false;
    Entry exefs, romfs;          // folders (romfs: the base's with the updates' files over it)
};

// True when `path` names a .nup file (by its extension, case-insensitive).
bool is_nup_path(const std::string& path);
// Reads the package's table of contents: the newest update's exefs (else the base's) and the romfs with every update laid over the
// base, in version order. False with *err set when the file is not a usable .nup.
bool read_program(const std::string& path, Program& out, std::string* err = nullptr);
// A file of `dir` by name, or null.
const Entry* find(const Entry& dir, const std::string& name);
// Writes the files directly in `dir` (not its sub folders) to `out_dir` (created), each through `<name>.part`. Files already there
// with the right size are kept. False with *err set at the first failure; zstd files are refused.
bool extract_files(const std::string& path, const Entry& dir, const std::string& out_dir, std::string* err = nullptr);
// Reads `len` bytes of a stored (codec 0) file at `ofs` in it. False when out of range or unreadable.
bool read_stored(const std::string& path, const Entry& file, uint64_t ofs, void* dst, uint64_t len);

}  // namespace nxrt::services::fs::nup
