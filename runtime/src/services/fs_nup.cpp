// SPDX-License-Identifier: GPL-3.0-or-later
// .nup reader (fs_nup.h). Layout as the launcher's nup.rs (ported from the fefw-recomp launcher, GPL-3.0): a 0x80-byte superblock
// ("NUP\x01", +0x08 TOC offset, +0x10 TOC size, +0x40 file size, +0x4C feature flags, must be 0); the TOC ("TOC\x01", +0x04 package
// count, +0x08 dir count, +0x10 file count, +0x18 string pool offset, +0x20 pool size) holds from +0x30 the packages (0x40 bytes: title
// id, root/exefs/romfs dir, version, kind 0 base / 1 update / 2 add-on at +0x18), the dirs (0x18: name offset, name length, first child
// slot, child count), the files (0x50: name offset, name length, size, extent offset, extent size, codec at +0x40), then the child
// slots (u32 each: a dir id, or dir count + file id) up to the string pool.
#define _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING
#include "fs_nup.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace nxrt::services::fs::nup {
namespace {

constexpr uint32_t kNoDir = 0xFFFFFFFFu;

struct Pkg {
    uint64_t title_id;
    uint32_t root, exefs, romfs, version;
    uint8_t kind;
};
struct RawDir {
    std::string name;
    uint32_t first, count;
};
struct RawFile {
    std::string name;
    uint64_t size, offset, stored;
    uint8_t codec;
};
struct Toc {
    uint64_t file_size = 0;
    std::vector<Pkg> pkgs;
    std::vector<RawDir> dirs;
    std::vector<RawFile> files;
    std::vector<uint32_t> slots;
};

bool fail(std::string* err, const std::string& m) {
    if (err) *err = m;
    return false;
}

bool get32(const std::vector<uint8_t>& b, size_t o, uint32_t& v) {
    if (o + 4 > b.size()) return false;
    std::memcpy(&v, b.data() + o, 4);
    return true;
}
bool get64(const std::vector<uint8_t>& b, size_t o, uint64_t& v) {
    if (o + 8 > b.size()) return false;
    std::memcpy(&v, b.data() + o, 8);
    return true;
}

std::filesystem::path host(const std::string& p) { return std::filesystem::u8path(p); }

bool read_at(std::ifstream& f, uint64_t ofs, void* dst, uint64_t n) {
    f.clear();
    f.seekg(static_cast<std::streamoff>(ofs));
    f.read(static_cast<char*>(dst), static_cast<std::streamsize>(n));
    return static_cast<uint64_t>(f.gcount()) == n;
}

bool parse(const std::string& path, Toc& t, std::string* err) {
    std::ifstream f(host(path), std::ios::binary);
    if (!f) return fail(err, "cannot open " + path);
    std::error_code ec;
    const uint64_t real = std::filesystem::file_size(host(path), ec);
    if (ec) return fail(err, "cannot read the size of " + path);
    std::vector<uint8_t> sb(0x80);
    if (!read_at(f, 0, sb.data(), sb.size())) return fail(err, path + " is too small to be a .nup package");
    if (std::memcmp(sb.data(), "NUP\x01", 4) != 0) return fail(err, path + " is not a .nup package (wrong header)");
    uint64_t toc_ofs = 0, toc_size = 0;
    uint32_t flags = 0;
    get64(sb, 0x08, toc_ofs);
    get64(sb, 0x10, toc_size);
    get64(sb, 0x40, t.file_size);
    get32(sb, 0x4C, flags);
    if (flags != 0) return fail(err, path + " uses .nup features this runtime does not know");
    if (t.file_size != real || toc_ofs > real || toc_size > real - toc_ofs || toc_size > (1u << 30))
        return fail(err, path + " is damaged or incomplete (its size does not match): copy it again");
    std::vector<uint8_t> toc(static_cast<size_t>(toc_size));
    if (!read_at(f, toc_ofs, toc.data(), toc.size()) || toc.size() < 0x30 || std::memcmp(toc.data(), "TOC\x01", 4) != 0)
        return fail(err, path + ": the table of contents is invalid");
    uint32_t npkg = 0;
    uint64_t ndir = 0, nfile = 0, pool_ofs = 0, pool_size = 0;
    get32(toc, 4, npkg);
    get64(toc, 8, ndir);
    get64(toc, 0x10, nfile);
    get64(toc, 0x18, pool_ofs);
    get64(toc, 0x20, pool_size);
    if (npkg > 4096 || ndir > toc.size() || nfile > toc.size() || pool_ofs > toc.size() || pool_size > toc.size() - pool_ofs)
        return fail(err, path + ": the table of contents is invalid");
    auto name_at = [&](size_t o, std::string& out) {
        uint32_t off = 0, len = 0;
        if (!get32(toc, o, off) || !get32(toc, o + 4, len) || off > pool_size || len > pool_size - off) return false;
        out.assign(reinterpret_cast<const char*>(toc.data() + pool_ofs + off), len);
        return true;
    };
    size_t pos = 0x30;
    for (uint32_t i = 0; i < npkg; i++, pos += 0x40) {
        Pkg p{};
        if (!get64(toc, pos, p.title_id) || !get32(toc, pos + 8, p.root) || !get32(toc, pos + 0xC, p.exefs) || !get32(toc, pos + 0x10, p.romfs) ||
            !get32(toc, pos + 0x14, p.version) || pos + 0x18 >= toc.size())
            return fail(err, path + ": the package list is truncated");
        p.kind = toc[pos + 0x18];
        t.pkgs.push_back(p);
    }
    for (uint64_t i = 0; i < ndir; i++, pos += 0x18) {
        RawDir d;
        if (!name_at(pos, d.name) || !get32(toc, pos + 8, d.first) || !get32(toc, pos + 0xC, d.count)) return fail(err, path + ": the folder list is truncated");
        t.dirs.push_back(std::move(d));
    }
    for (uint64_t i = 0; i < nfile; i++, pos += 0x50) {
        RawFile r;
        if (!name_at(pos, r.name) || !get64(toc, pos + 8, r.size) || !get64(toc, pos + 0x10, r.offset) || !get64(toc, pos + 0x18, r.stored) ||
            pos + 0x40 >= toc.size())
            return fail(err, path + ": the file list is truncated");
        r.codec = toc[pos + 0x40];
        t.files.push_back(std::move(r));
    }
    if (pool_ofs < pos || (pool_ofs - pos) % 4) return fail(err, path + ": the table of contents is invalid");
    for (; pos < pool_ofs; pos += 4) {
        uint32_t v = 0;
        get32(toc, pos, v);
        t.slots.push_back(v);
    }
    return true;
}

bool safe(const std::string& n) {
    return !n.empty() && n != "." && n != ".." && n.find_first_of("/\\:") == std::string::npos && n.find('\0') == std::string::npos;
}

// The folder `d` as an Entry tree.
bool tree(const Toc& t, uint32_t d, Entry& out, int depth, std::string* err) {
    if (depth > 64) return fail(err, "the .nup folder tree is too deep");
    if (d >= t.dirs.size()) return fail(err, "the .nup refers to a folder that does not exist");
    const RawDir& rd = t.dirs[d];
    out.dir = true;
    const uint64_t ndir = t.dirs.size();
    for (uint64_t s = rd.first; s < uint64_t(rd.first) + rd.count; s++) {
        if (s >= t.slots.size()) return fail(err, "the .nup folder listing is out of bounds");
        const uint32_t id = t.slots[s];
        Entry k;
        if (id < ndir) {
            k.name = t.dirs[id].name;
            if (!tree(t, id, k, depth + 1, err)) return false;
        } else {
            if (id - ndir >= t.files.size()) return fail(err, "the .nup refers to a file that does not exist");
            const RawFile& f = t.files[id - ndir];
            k.name = f.name;
            k.size = f.size;
            k.offset = f.offset;
            k.stored = f.stored;
            k.codec = f.codec;
            if (f.offset > t.file_size || f.stored > t.file_size - f.offset) return fail(err, f.name + " points outside the .nup file");
        }
        if (!safe(k.name)) return fail(err, "the .nup has an unsafe name: " + k.name);
        out.kids.push_back(std::move(k));
    }
    std::sort(out.kids.begin(), out.kids.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
    return true;
}

// Lays `top` over `base`: a file replaces the one of the same name, folders merge.
void overlay(Entry& base, Entry&& top) {
    for (Entry& k : top.kids) {
        auto it = std::find_if(base.kids.begin(), base.kids.end(), [&](const Entry& b) { return b.name == k.name; });
        if (it == base.kids.end()) {
            base.kids.push_back(std::move(k));
        } else if (it->dir && k.dir) {
            overlay(*it, std::move(k));
        } else {
            *it = std::move(k);
        }
    }
    std::sort(base.kids.begin(), base.kids.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
}

}  // namespace

bool is_nup_path(const std::string& p) {
    if (p.size() < 4) return false;
    std::string e = p.substr(p.size() - 4);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e == ".nup";
}

bool read_program(const std::string& path, Program& out, std::string* err) {
    Toc t;
    if (!parse(path, t, err)) return false;
    std::vector<const Pkg*> order;   // the base, then the updates by version
    for (const Pkg& p : t.pkgs)
        if (p.kind == 0 || p.kind == 1) order.push_back(&p);
    std::stable_sort(order.begin(), order.end(), [](const Pkg* a, const Pkg* b) {
        return a->kind != b->kind ? a->kind < b->kind : a->version < b->version;
    });
    if (order.empty()) return fail(err, path + " has no base game or update in it");
    out = Program{};
    for (const Pkg* p : order) {
        if (p->exefs != kNoDir) {
            Entry e;
            if (!tree(t, p->exefs, e, 0, err)) return false;
            out.exefs = std::move(e);
            out.has_exefs = true;
            out.title_id = p->title_id;
            out.version = p->version;
            out.update = p->kind == 1;
        }
        if (p->romfs != kNoDir) {
            Entry r;
            if (!tree(t, p->romfs, r, 0, err)) return false;
            if (out.has_romfs) overlay(out.romfs, std::move(r));
            else out.romfs = std::move(r);
            out.has_romfs = true;
        }
    }
    if (!out.has_exefs || !out.has_romfs) return fail(err, path + " has no game program (exefs and romfs)");
    return true;
}

const Entry* find(const Entry& dir, const std::string& name) {
    for (const Entry& k : dir.kids)
        if (k.name == name) return &k;
    return nullptr;
}

bool read_stored(const std::string& path, const Entry& file, uint64_t ofs, void* dst, uint64_t len) {
    if (file.dir || file.codec != 0 || ofs > file.size || len > file.size - ofs) return false;
    std::ifstream f(host(path), std::ios::binary);
    return f && read_at(f, file.offset + ofs, dst, len);
}

bool extract_files(const std::string& path, const Entry& dir, const std::string& out_dir, std::string* err) {
    std::error_code ec;
    std::filesystem::create_directories(host(out_dir), ec);
    std::ifstream in(host(path), std::ios::binary);
    if (!in) return fail(err, "cannot open " + path);
    std::vector<char> buf(4u << 20);
    for (const Entry& k : dir.kids) {
        if (k.dir) continue;
        const std::string dest_s = out_dir + "/" + k.name, part_s = dest_s + ".part";
        const std::filesystem::path dest = host(dest_s);
        if (std::filesystem::is_regular_file(dest, ec) && std::filesystem::file_size(dest, ec) == k.size && !ec) continue;
        if (k.codec != 0) return fail(err, k.name + " is compressed in the .nup: unpack the .nup on a PC instead");
        const std::filesystem::path part = host(part_s);
        {
            std::ofstream o(part, std::ios::binary | std::ios::trunc);
            if (!o) return fail(err, "cannot write " + part_s);
            uint64_t done = 0;
            while (done < k.size) {
                const uint64_t n = std::min<uint64_t>(buf.size(), k.size - done);
                if (!read_at(in, k.offset + done, buf.data(), n)) return fail(err, "cannot read " + k.name + " from " + path);
                o.write(buf.data(), static_cast<std::streamsize>(n));
                if (!o) return fail(err, "cannot write " + part_s);
                done += n;
            }
        }
        std::filesystem::rename(part, dest, ec);
        if (ec) return fail(err, "cannot write " + dest_s);
    }
    return true;
}

}  // namespace nxrt::services::fs::nup
