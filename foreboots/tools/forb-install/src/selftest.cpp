// selftest.cpp - offline tests against the tools/tests fixtures + payload.
#include "forb/forb.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include <filesystem>
#include <unistd.h>
#include <zlib.h>

namespace fs = std::filesystem;
using namespace forb;

#ifndef FORB_TESTS_DIR
#define FORB_TESTS_DIR "tests"
#endif

namespace {

void require(bool cond, const std::string& msg) {
    if (!cond) throw std::runtime_error(msg);
}

std::string tests_dir() { return FORB_TESTS_DIR; }

struct TempDir {
    fs::path path;
    TempDir() {
        static int counter = 0;
        path = fs::temp_directory_path() /
               ("forb-selftest-" + std::to_string(::getpid()) + "-" +
                std::to_string(counter++));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

Args make_args(const std::string& esp, const std::string& config,
               std::optional<int> default_entry = std::nullopt,
               int max_entries = DEFAULT_MAX_ENTRIES, bool no_extras = false) {
    Args a;
    a.esp = esp;
    a.config = config;
    a.default_entry = default_entry;
    a.max_entries = max_entries;
    a.no_extras = no_extras;
    a.dry_run = true;
    a.no_nvram = true;
    return a;
}

BuildResult build(const std::string& config, const std::string& esp,
                  std::optional<int> default_entry = std::nullopt,
                  int max_entries = DEFAULT_MAX_ENTRIES,
                  bool no_extras = false) {
    Args a = make_args(esp, config, default_entry, max_entries, no_extras);
    Reporter rep(false);
    auto res = build_config(a, rep);
    require(res.has_value(), "build_config failed");
    return *res;
}

// ---------------------------------------------------------------------------
//  PNG builder (color type 2), row y uses filters[y % len].
// ---------------------------------------------------------------------------
void be32(std::string& o, uint32_t v) {
    o.push_back(char((v >> 24) & 0xFF));
    o.push_back(char((v >> 16) & 0xFF));
    o.push_back(char((v >> 8) & 0xFF));
    o.push_back(char(v & 0xFF));
}

std::string make_chunk(const char* typ, const std::string& data) {
    std::string out;
    be32(out, static_cast<uint32_t>(data.size()));
    std::string td = std::string(typ, 4) + data;
    out += td;
    uLong crc = crc32(0L, reinterpret_cast<const Bytef*>(td.data()),
                      static_cast<uInt>(td.size()));
    be32(out, static_cast<uint32_t>(crc));
    return out;
}

std::string make_test_png(int w, int h,
                          const std::function<std::array<int, 3>(int, int)>& px,
                          const std::vector<int>& filters) {
    int stride = w * 3;
    std::string raw;
    std::vector<unsigned char> prev(stride, 0);
    for (int y = 0; y < h; ++y) {
        std::vector<unsigned char> cur(stride);
        for (int x = 0; x < w; ++x) {
            auto p = px(x, y);
            cur[x * 3 + 0] = static_cast<unsigned char>(p[0]);
            cur[x * 3 + 1] = static_cast<unsigned char>(p[1]);
            cur[x * 3 + 2] = static_cast<unsigned char>(p[2]);
        }
        int f = filters[y % static_cast<int>(filters.size())];
        std::string enc(stride, '\0');
        for (int i = 0; i < stride; ++i) {
            int a = i >= 3 ? cur[i - 3] : 0;
            int b = prev[i];
            int c = i >= 3 ? prev[i - 3] : 0;
            int x = cur[i];
            int e;
            if (f == 0) e = x;
            else if (f == 1) e = (x - a) & 0xFF;
            else if (f == 2) e = (x - b) & 0xFF;
            else if (f == 3) e = (x - ((a + b) >> 1)) & 0xFF;
            else e = (x - paeth(a, b, c)) & 0xFF;
            enc[i] = static_cast<char>(e);
        }
        raw.push_back(static_cast<char>(f));
        raw += enc;
        prev = cur;
    }
    // zlib compress raw
    uLongf bound = compressBound(static_cast<uLong>(raw.size()));
    std::string comp(bound, '\0');
    uLongf clen = bound;
    compress(reinterpret_cast<Bytef*>(&comp[0]), &clen,
             reinterpret_cast<const Bytef*>(raw.data()),
             static_cast<uLong>(raw.size()));
    comp.resize(clen);

    std::string ihdr;
    be32(ihdr, static_cast<uint32_t>(w));
    be32(ihdr, static_cast<uint32_t>(h));
    ihdr.push_back(8);   // bit depth
    ihdr.push_back(2);   // color type
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);

    std::string out;
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n',
                                         0x1a, '\n'};
    out.append(reinterpret_cast<const char*>(sig), 8);
    out += make_chunk("IHDR", ihdr);
    out += make_chunk("IDAT", comp);
    out += make_chunk("IEND", "");
    return out;
}

uint32_t le32(const std::string& s, size_t off) {
    return (uint8_t(s[off])) | (uint8_t(s[off + 1]) << 8) |
           (uint8_t(s[off + 2]) << 16) | (uint32_t(uint8_t(s[off + 3])) << 24);
}
int32_t le32s(const std::string& s, size_t off) {
    return static_cast<int32_t>(le32(s, off));
}

}  // namespace

int forb::cmd_selftest() {
    std::vector<std::pair<std::string, std::string>> results;
    auto check = [&](const std::string& name, const std::function<void()>& fn) {
        try {
            fn();
            results.emplace_back(name, "");
        } catch (const std::exception& e) {
            results.emplace_back(name, std::string(e.what()));
        } catch (const PngError& e) {
            results.emplace_back(name, "PngError: " + e.msg);
        } catch (...) {
            results.emplace_back(name, "unknown error");
        }
    };
    std::string tdir = tests_dir();

    // -- 1. limine fixture tree ---------------------------------------------
    check("limine-tree", [&] {
        TempDir esp;
        auto res = build((fs::path(tdir) / "limine.conf").string(),
                         esp.path.string());
        auto& roots = res.roots;
        require(roots.size() == 3, "expected 3 top-level nodes, got " +
                std::to_string(roots.size()));
        require(roots[0].is_group() && roots[0].group->title == "CachyOS",
                "root0 CachyOS");
        std::vector<std::string> names;
        for (auto& c : roots[0].group->children)
            names.push_back(c.is_group() ? c.group->title : c.entry->title);
        std::vector<std::string> want = {"linux-cachyos",
                                         "linux-cachyos-rc-gcc",
                                         "linux-cachyos-lts", "Snapshot 906",
                                         "Snapshot 905"};
        require(names == want, "CachyOS children mismatch");
        int nsnap = 0;
        std::vector<const OutGroup*> snaps;
        for (auto& c : roots[0].group->children)
            if (c.is_group()) { snaps.push_back(c.group.get()); ++nsnap; }
        require(nsnap == 2, "2 snapshot groups");
        require(snaps[0]->children.size() == 3, "Snapshot 906 children");
        require(snaps[1]->children.size() == 1, "Snapshot 905 children");
        require(roots[1].is_group() &&
                roots[1].group->title == "Other systems and bootloaders",
                "root1 Other systems");
        require(roots[1].group->children.size() == 1, "Other systems 1 child");
        require(!roots[2].is_group() && roots[2].entry->title == "EFI fallback",
                "root2 EFI fallback");
        require(roots[2].entry->type == "chainload", "EFI fallback chainload");
    });

    // -- 2. default_entry=2 -> title path -----------------------------------
    check("limine-default-path", [&] {
        TempDir esp;
        auto res = build((fs::path(tdir) / "limine.conf").string(),
                         esp.path.string());
        require(res.default_str == "CachyOS/linux-cachyos-rc-gcc",
                "default_str=" + res.default_str);
    });

    // -- 3. generated content ------------------------------------------------
    check("limine-content", [&] {
        TempDir esp;
        auto res = build((fs::path(tdir) / "limine.conf").string(),
                         esp.path.string());
        const std::string& cfg = res.cfg_text;
        for (const char* needle :
             {"timeout=5", "remember_last=1",
              "default=CachyOS/linux-cachyos-rc-gcc", "submenu \"CachyOS\"",
              "submenu \"Snapshot 906\"", "type=linux",
              "chain=/efi/Microsoft/Boot/bootmgfw.efi",
              "rootflags=subvol=/@/.snapshots/906/snapshot",
              "menuentry \"ForeB Shell\"",
              "menuentry \"Recovery / Disk Tools\"", "menuentry \"Tools\"",
              "menuentry \"Firmware Setup (UEFI)\"", "menuentry \"Reboot\""})
            require(cfg.find(needle) != std::string::npos,
                    std::string("missing: ") + needle);
        for (const auto& line : splitlines(cfg)) {
            std::string s = strip(line);
            if (starts_with(s, "vmlinuz=") || starts_with(s, "initrd=") ||
                starts_with(s, "kernel=") || starts_with(s, "module=") ||
                starts_with(s, "chain="))
                require(s.find('#') == std::string::npos,
                        "hash left in path: " + line);
        }
        require(cfg.find("_sha256_") != std::string::npos,
                "_sha256_ was stripped but must stay");
        require(cfg.find("vmlinuz-linux-cachyos_sha256_2c233057") !=
                std::string::npos, "sha256 name missing");
        auto res2 = build((fs::path(tdir) / "limine.conf").string(),
                          esp.path.string(), std::nullopt, DEFAULT_MAX_ENTRIES,
                          true);
        require(res2.cfg_text.find("menuentry \"ForeB Shell\"") ==
                std::string::npos, "extras present with no_extras");
    });

    // -- 4. firmware length limits -------------------------------------------
    check("firmware-length-limits", [&] {
        TempDir esp;
        auto res = build((fs::path(tdir) / "limine.conf").string(),
                         esp.path.string());
        for (const auto& line : splitlines(res.cfg_text)) {
            std::string s = strip(line);
            for (const char* key : {"vmlinuz=", "initrd=", "kernel=",
                                    "module=", "chain="}) {
                if (starts_with(s, key)) {
                    std::string val = s.substr(std::strlen(key));
                    require((int)val.size() <= MAX_PATH,
                            std::string(key) + " too long");
                }
            }
            if (starts_with(s, "cmdline=\"")) {
                std::string val = s.substr(std::strlen("cmdline=\""));
                if (!val.empty() && val.back() == '"') val.pop_back();
                require((int)val.size() <= MAX_CMDLINE, "cmdline too long");
            }
        }
    });

    // -- 5. grub fixture -----------------------------------------------------
    check("grub-fixture", [&] {
        TempDir esp;
        auto res = build((fs::path(tdir) / "grub.cfg").string(),
                         esp.path.string());
        auto entries = flatten_entries(res.roots);
        require(entries.size() == 3, "expected 3 entries, got " +
                std::to_string(entries.size()));
        require(res.n_submenus == 1, "submenus=" +
                std::to_string(res.n_submenus));
        std::vector<std::string> types;
        for (auto& [e, p] : entries) { (void)p; types.push_back(e->type); }
        require((types == std::vector<std::string>{"linux", "linux",
                "chainload"}), "types mismatch");
        require(res.default_str == "Ubuntu 24.04 LTS",
                "default_str=" + res.default_str);
        require(res.cfg_text.find("timeout=10") != std::string::npos,
                "timeout=10 missing");
        std::shared_ptr<OutEntry> win;
        for (auto& [e, p] : entries) { (void)p; if (e->type == "chainload") win = e; }
        require(win && win->chain == "/EFI/Microsoft/Boot/bootmgfw.efi",
                "win chain");
        require(win->icon == "windows", "win icon");
        require(entries[0].first->icon == "ubuntu", "ubuntu icon");
    });

    // -- 6. systemd-boot fixture --------------------------------------------
    check("systemd-boot-fixture", [&] {
        TempDir esp;
        fs::copy(fs::path(tdir) / "loader", esp.path / "loader",
                 fs::copy_options::recursive);
        auto res = build((esp.path / "loader" / "loader.conf").string(),
                         esp.path.string());
        auto entries = flatten_entries(res.roots);
        require(entries.size() == 3, "expected 3 entries, got " +
                std::to_string(entries.size()));
        std::vector<std::string> types;
        for (auto& [e, p] : entries) { (void)p; types.push_back(e->type); }
        require((types == std::vector<std::string>{"linux", "linux",
                "chainload"}), "types mismatch");
        require(res.default_str == "Arch Linux",
                "default_str=" + res.default_str);
        require(res.cfg_text.find("timeout=3") != std::string::npos,
                "timeout=3 missing");
        require(entries[1].first->icon == "arch", "arch icon");
        require(entries[2].first->chain == "/EFI/Microsoft/Boot/bootmgfw.efi",
                "windows chain");
    });

    // -- 7. PNG round-trip + BMP header --------------------------------------
    check("png-bmp-roundtrip", [&] {
        int w = 4, h = 5;
        auto px = [](int x, int y) -> std::array<int, 3> {
            return {(x * 40 + y * 7) % 256, (x * 3 + y * 50) % 256,
                    (x * 90 + y * 11) % 256};
        };
        std::string png = make_test_png(w, h, px, {0, 1, 2, 3, 4});
        Image img = png_decode(png);
        require(img.w == w && img.h == h, "png dims");
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                auto want = px(x, y);
                require(uint8_t(img.rows[y][x * 3 + 0]) == want[0] &&
                        uint8_t(img.rows[y][x * 3 + 1]) == want[1] &&
                        uint8_t(img.rows[y][x * 3 + 2]) == want[2],
                        "pixel mismatch");
            }
        std::string bmp = bmp_encode(img.w, img.h, img.rows);
        require(bmp[0] == 'B' && bmp[1] == 'M', "bmp magic");
        require(le32(bmp, 2) == bmp.size(), "bmp file size");
        uint32_t off = le32(bmp, 10);
        require(le32s(bmp, 18) == w && le32s(bmp, 22) == h, "bmp dims");
        require((uint8_t(bmp[26]) | (uint8_t(bmp[27]) << 8)) == 1, "planes");
        require((uint8_t(bmp[28]) | (uint8_t(bmp[29]) << 8)) == 24, "bpp");
        int row_bytes = w * 3;
        int pad = (4 - row_bytes % 4) % 4;
        require((int)(bmp.size() - off) == (row_bytes + pad) * h, "bmp size");
        auto bl = px(0, h - 1);
        require(uint8_t(bmp[off + 0]) == bl[2] &&
                uint8_t(bmp[off + 1]) == bl[1] &&
                uint8_t(bmp[off + 2]) == bl[0], "bottom-left BGR");
        bool raised = false;
        try { png_decode("not a png"); } catch (const PngError&) { raised = true; }
        require(raised, "bad PNG not rejected");
    });

    // -- 8. limine path resolution ------------------------------------------
    check("limine-path-resolution", [&] {
        Reporter rep;
        EspContext ctx("/nonexistent-esp", rep);
        auto rp = [&](const std::string& v) {
            return resolve_limine_path(v, ctx, rep);
        };
        std::string ab64;
        for (int i = 0; i < 64; ++i) ab64 += "ab";
        std::string ab32;
        for (int i = 0; i < 32; ++i) ab32 += "ab";
        require(rp("boot():/foo/bar#" + ab64) == "/foo/bar", "hash strip");
        require(rp("boot():/limine_history/vmlinuz-x_sha256_" + ab32 + "#" +
                   ab64) == "/limine_history/vmlinuz-x_sha256_" + ab32,
                "sha256 kept");
        require(rp("guid():/efi/Microsoft/Boot/bootmgfw.efi") ==
                "/efi/Microsoft/Boot/bootmgfw.efi", "guid");
        require(rp("uuid():\\EFI\\BOOT\\BOOTX64.EFI") ==
                "/EFI/BOOT/BOOTX64.EFI", "uuid backslash");
        require(rp("/plain/path") == "/plain/path", "plain");
        require(rp("no-slash-path") == "/no-slash-path", "no-slash");
        require(rp("hdd(1,2):/x") == "/x", "hdd");
        bool found = false;
        for (auto& w : rep.warnings)
            if (w.find("unsupported scheme") != std::string::npos) found = true;
        require(found, "unsupported scheme warning");
    });

    // -- 9. icon guessing ----------------------------------------------------
    check("icon-guessing", [&] {
        require(guess_icon("linux-cachyos vmlinuz-linux-cachyos", "linux") ==
                "arch", "cachyos->arch");
        require(guess_icon("Windows Boot Manager", "chainload") == "windows",
                "windows");
        require(guess_icon("EFI fallback", "chainload") == "usb", "efi fb");
        require(guess_icon("Snapshot 906") == "safe", "snapshot");
        require(guess_icon("CachyOS") == "arch", "CachyOS");
        require(guess_icon("Other systems and bootloaders") == "", "empty");
        require(guess_icon("Some Distro kernel", "linux") == "tux", "tux");
        require(guess_icon("Some app", "chainload") == "usb", "usb fallback");
        require(guess_icon("Ubuntu 24.04 LTS ubuntu", "linux") == "ubuntu",
                "ubuntu");
    });

    // -- 10. wallpaper handling ---------------------------------------------
    check("wallpaper", [&] {
        {
            TempDir esp;
            auto png = make_test_png(3, 2,
                [](int x, int y) -> std::array<int, 3> {
                    return {x, y, x * y};
                }, {0, 2});
            std::ofstream f((esp.path / "limine-splash.png").string(),
                            std::ios::binary);
            f.write(png.data(), png.size());
            f.close();
            auto res = build((fs::path(tdir) / "limine.conf").string(),
                             esp.path.string());
            require(res.cfg_text.find("background=/forebo/wallpaper.bmp") !=
                    std::string::npos, "background line");
            require(res.wallpaper.has_value(), "wallpaper job");
            require(res.wallpaper->first == "wallpaper.bmp", "dest name");
            const std::string& data = res.wallpaper->second;
            require(data[0] == 'B' && data[1] == 'M', "bmp magic");
            require(le32s(data, 18) == 3 && le32s(data, 22) == 2, "dims");
        }
        {
            TempDir esp;
            Args a = make_args(esp.path.string(),
                               (fs::path(tdir) / "limine.conf").string());
            Reporter rep(false);
            auto res = build_config(a, rep);
            require(res.has_value(), "build failed");
            require(res->cfg_text.find("background=") == std::string::npos,
                    "no background");
            bool warned = false;
            for (auto& w : rep.warnings)
                if (w.find("wallpaper") != std::string::npos) warned = true;
            require(warned, "missing wallpaper warning");
        }
    });

    // -- 11. title sanitization ---------------------------------------------
    check("title-sanitization", [&] {
        require(sanitize_title("A/B \"C\"") == "A\xE2\x88\x95""B 'C'",
                "sanitize");
        Reporter rep;
        EspContext ctx("/nonexistent-esp", rep);
        std::string text =
            "timeout: 1\n/a/b\n    protocol: linux\n    path: boot():/vmlinuz\n";
        TempDir td;
        std::string p = (td.path / "limine.conf").string();
        std::ofstream f(p);
        f << text;
        f.close();
        auto cfg = parse_limine(p, ctx, rep);
        require(!cfg.roots.empty() && !cfg.roots[0].is_group(),
                "root is entry");
        require(cfg.roots[0].entry->title == "a\xE2\x88\x95""b", "title");
    });

    // -- 12. embedded payload ------------------------------------------------
    check("embedded-payload", [&] {
        const auto& files = payload_files();
        bool have_loader = false, have_cfg = false;
        int icons = 0;
        for (const auto& pf : files) {
            require(!pf.data.empty(), "empty payload file: " + pf.name);
            if (pf.name == "EFI/forb/BOOTX64.EFI") have_loader = true;
            if (pf.name == "forebo/forebo.cfg") have_cfg = true;
            if (starts_with(pf.name, "forebo/icons/") &&
                ends_with(pf.name, ".tga"))
                ++icons;
        }
        require(have_loader, "payload missing EFI/forb/BOOTX64.EFI");
        require(have_cfg, "payload missing forebo/forebo.cfg");
        require(icons >= 18, "payload has only " + std::to_string(icons) +
                " icons (need >=18)");
    });

    // -- report --------------------------------------------------------------
    int n_fail = 0;
    for (auto& [name, err] : results) {
        if (err.empty()) std::cout << "PASS " << name << "\n";
        else { std::cout << "FAIL " << name << ": " << err << "\n"; ++n_fail; }
    }
    std::cout << (results.size() - n_fail) << "/" << results.size()
              << " tests passed\n";
    return n_fail == 0 ? 0 : 1;
}
