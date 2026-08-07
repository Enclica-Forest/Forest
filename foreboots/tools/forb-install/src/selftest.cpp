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

    // =========================================================================
    //  NEW COMPREHENSIVE SELF-TESTS
    // =========================================================================

    // -- 13. syslinux parser --------------------------------------------------
    check("syslinux-basic", [&] {
        TempDir td;
        std::string cfg =
            "TIMEOUT 50\n"
            "DEFAULT linux-label\n"
            "\n"
            "LABEL linux-label\n"
            "    MENU LABEL My Linux\n"
            "    KERNEL /vmlinuz-linux\n"
            "    APPEND root=/dev/sda1 rw\n"
            "    INITRD /initramfs-linux.img\n"
            "\n"
            "LABEL live-label\n"
            "    MENU LABEL Live USB\n"
            "    KERNEL /boot/bzImage\n"
            "    APPEND live\n"
            "\n"
            "LABEL local\n"
            "    MENU LABEL Local Disk\n"
            "    COM32 /EFI/LocalBoot.efi\n"
            "\n";
        std::string p = (td.path / "syslinux.cfg").string();
        std::ofstream f(p);
        f << cfg;
        f.close();
        Reporter rep;
        EspContext ctx(td.path.string(), rep);
        auto parsed = parse_syslinux(p, ctx, rep);
        require(parsed.kind == "syslinux", "kind=syslinux");
        auto entries = flatten_entries(parsed.roots);
        require(entries.size() == 3,
                "expected 3 entries, got " + std::to_string(entries.size()));
        require(entries[0].first->type == "linux", "entry0 type=linux");
        require(entries[0].first->title == "My Linux", "entry0 title");
        require(entries[0].first->vmlinuz == "/vmlinuz-linux",
                "entry0 vmlinuz");
        require(entries[0].first->initrd == "/initramfs-linux.img",
                "entry0 initrd");
        require(entries[0].first->cmdline == "root=/dev/sda1 rw",
                "entry0 cmdline");
        require(entries[1].first->type == "linux", "entry1 type=linux");
        require(entries[1].first->title == "Live USB", "entry1 title");
        require(entries[1].first->vmlinuz == "/boot/bzImage",
                "entry1 vmlinuz");
        require(entries[2].first->type == "chainload", "entry2 type=chainload");
        require(entries[2].first->title == "Local Disk", "entry2 title");
        require(entries[2].first->chain == "/EFI/LocalBoot.efi",
                "entry2 chain");
    });

    // -- 13b. syslinux timeout conversion ------------------------------------
    check("syslinux-timeout", [&] {
        TempDir td;
        std::string cfg =
            "TIMEOUT 30\n"
            "\n"
            "LABEL test\n"
            "    KERNEL /vmlinuz\n"
            "\n";
        std::string p = (td.path / "syslinux.cfg").string();
        std::ofstream f(p);
        f << cfg;
        f.close();
        Reporter rep;
        EspContext ctx(td.path.string(), rep);
        auto parsed = parse_syslinux(p, ctx, rep);
        require(parsed.timeout.has_value(), "timeout set");
        require(*parsed.timeout == 3, "timeout=30/10=3");
    });

    // -- 13c. syslinux default label resolution ------------------------------
    check("syslinux-default-label", [&] {
        TempDir td;
        std::string cfg =
            "DEFAULT second\n"
            "\n"
            "LABEL first\n"
            "    KERNEL /vmlinuz-1\n"
            "\n"
            "LABEL second\n"
            "    KERNEL /vmlinuz-2\n"
            "\n";
        std::string p = (td.path / "syslinux.cfg").string();
        std::ofstream f(p);
        f << cfg;
        f.close();
        Reporter rep;
        EspContext ctx(td.path.string(), rep);
        auto parsed = parse_syslinux(p, ctx, rep);
        require(parsed.def.kind == DefaultSpec::Index,
                "default resolved to index");
        require(parsed.def.index == 2, "default index=2 (second label)");
    });

    // -- 14. rEFInd parser ---------------------------------------------------
    check("refind-basic", [&] {
        TempDir td;
        std::string cfg =
            "timeout 5\n"
            "default_selection 1\n"
            "\n"
            "menuentry \"Linux\" {\n"
            "    loader /EFI/vmlinuz\n"
            "    options root=/dev/sda1 rw\n"
            "    initrd /EFI/initrd.img\n"
            "    icon /EFI/icons/linux.icn\n"
            "}\n"
            "\n"
            "menuentry \"Windows\" {\n"
            "    loader /EFI/Microsoft/Boot/bootmgfw.efi\n"
            "    icon /EFI/icons/win.icn\n"
            "}\n"
            "\n"
            "menuentry \"Disabled Entry\" {\n"
            "    loader /EFI/old.efi\n"
            "    disabled\n"
            "}\n"
            "\n";
        std::string p = (td.path / "refind.conf").string();
        std::ofstream f(p);
        f << cfg;
        f.close();
        Reporter rep;
        EspContext ctx(td.path.string(), rep);
        auto parsed = parse_refind(p, ctx, rep);
        require(parsed.kind == "refind", "kind=refind");
        require(parsed.timeout.has_value(), "timeout set");
        require(*parsed.timeout == 5, "timeout=5");
        auto entries = flatten_entries(parsed.roots);
        require(entries.size() == 2, "disabled entry skipped, 2 entries");
        require(entries[0].first->title == "Linux", "entry0 title=Linux");
        require(entries[0].first->type == "linux", "entry0 type=linux");
        require(entries[0].first->vmlinuz == "/EFI/vmlinuz",
                "entry0 loader");
        require(entries[0].first->cmdline == "root=/dev/sda1 rw",
                "entry0 options");
        require(entries[0].first->initrd == "/EFI/initrd.img",
                "entry0 initrd");
        require(entries[1].first->title == "Windows", "entry1 title=Windows");
        require(entries[1].first->type == "chainload",
                "entry1 type=chainload");
        require(entries[1].first->chain == "/EFI/Microsoft/Boot/bootmgfw.efi",
                "entry1 loader");
    });

    // -- 14b. rEFInd submenuentry -------------------------------------------
    check("refind-submenuentry", [&] {
        TempDir td;
        std::string cfg =
            "menuentry \"Linux\" {\n"
            "    loader /EFI/vmlinuz\n"
            "    submenuentry \"recovery\" {\n"
            "        loader /EFI/vmlinuz-recovery\n"
            "        options root=/dev/sda2\n"
            "    }\n"
            "}\n"
            "\n";
        std::string p = (td.path / "refind.conf").string();
        std::ofstream f(p);
        f << cfg;
        f.close();
        Reporter rep;
        EspContext ctx(td.path.string(), rep);
        auto parsed = parse_refind(p, ctx, rep);
        auto entries = flatten_entries(parsed.roots);
        require(entries.size() == 2,
                "expected 2 entries (main + submenu), got " +
                std::to_string(entries.size()));
        require(entries[0].first->title == "Linux", "main entry title");
        require(entries[1].first->title == "recovery", "submenu title");
        require(entries[1].first->cmdline == "root=/dev/sda2",
                "submenu options");
    });

    // -- 15. Clover parser ---------------------------------------------------
    check("clover-plist", [&] {
        TempDir td;
        std::string plist =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
            "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
            "<plist version=\"1.0\">\n"
            "<dict>\n"
            "    <key>GUI</key>\n"
            "    <dict>\n"
            "        <key>Timeout</key>\n"
            "        <integer>8</integer>\n"
            "        <key>DefaultVolume</key>\n"
            "        <string>Ubuntu</string>\n"
            "        <key>Entries</key>\n"
            "        <array>\n"
            "            <dict>\n"
            "                <key>Title</key>\n"
            "                <string>Arch Linux</string>\n"
            "                <key>Type</key>\n"
            "                <integer>0</integer>\n"
            "                <key>Volume</key>\n"
            "                <string>UEFI</string>\n"
            "                <key>Kernel</key>\n"
            "                <string>/EFI/arch/vmlinuz</string>\n"
            "                <key>KernelFlags</key>\n"
            "                <string>root=PARTUUID=xxx rw</string>\n"
            "            </dict>\n"
            "            <dict>\n"
            "                <key>Title</key>\n"
            "                <string>Windows</string>\n"
            "                <key>Type</key>\n"
            "                <integer>2</integer>\n"
            "                <key>Volume</key>\n"
            "                <string>Windows</string>\n"
            "                <key>Path</key>\n"
            "                <string>\\EFI\\Microsoft\\Boot\\bootmgfw.efi</string>\n"
            "            </dict>\n"
            "            <dict>\n"
            "                <key>Title</key>\n"
            "                <string>macOS</string>\n"
            "                <key>Type</key>\n"
            "                <integer>1</integer>\n"
            "                <key>Path</key>\n"
            "                <string>\\EFI\\Apple\\Boot\\boot.efi</string>\n"
            "            </dict>\n"
            "        </array>\n"
            "    </dict>\n"
            "</dict>\n"
            "</plist>\n";
        std::string p = (td.path / "config.plist").string();
        std::ofstream f(p);
        f << plist;
        f.close();
        Reporter rep;
        EspContext ctx(td.path.string(), rep);
        auto parsed = parse_clover(p, ctx, rep);
        require(parsed.kind == "clover", "kind=clover");
        require(parsed.timeout.has_value(), "timeout set");
        require(*parsed.timeout == 8, "timeout=8");
        auto entries = flatten_entries(parsed.roots);
        require(entries.size() == 2,
                "expected 2 entries (macOS type=1 skipped?), got " +
                std::to_string(entries.size()));
        require(entries[0].first->title == "Arch Linux", "entry0 title");
        require(entries[0].first->type == "linux", "type=0->linux");
        require(entries[0].first->vmlinuz == "/EFI/arch/vmlinuz",
                "entry0 kernel path");
        require(entries[0].first->cmdline == "root=PARTUUID=xxx rw",
                "entry0 kernel flags");
        require(entries[1].first->title == "Windows", "entry1 title");
        require(entries[1].first->type == "chainload", "type=2->chainload");
        require(entries[1].first->chain ==
                "/EFI/Microsoft/Boot/bootmgfw.efi", "entry1 path");
    });

    // -- 15b. clover volume + path combination ------------------------------
    check("clover-volume-path", [&] {
        TempDir td;
        std::string plist =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<plist version=\"1.0\">\n"
            "<dict>\n"
            "    <key>GUI</key>\n"
            "    <dict>\n"
            "        <key>Entries</key>\n"
            "        <array>\n"
            "            <dict>\n"
            "                <key>Title</key>\n"
            "                <string>Test</string>\n"
            "                <key>Type</key>\n"
            "                <integer>0</integer>\n"
            "                <key>Volume</key>\n"
            "                <string>BOOT</string>\n"
            "                <key>Path</key>\n"
            "                <string>\\EFI\\test\\boot.efi</string>\n"
            "            </dict>\n"
            "        </array>\n"
            "    </dict>\n"
            "</dict>\n"
            "</plist>\n";
        std::string p = (td.path / "config.plist").string();
        std::ofstream f(p);
        f << plist;
        f.close();
        Reporter rep;
        EspContext ctx(td.path.string(), rep);
        auto parsed = parse_clover(p, ctx, rep);
        auto entries = flatten_entries(parsed.roots);
        require(entries.size() == 1, "1 entry");
        require(entries[0].first->chain == "/EFI/test/boot.efi",
                "path normalized from backslash");
    });

    // -- 16. GRUB variable expansion ($root, ${root}) -----------------------
    check("grub-variable-expansion", [&] {
        TempDir td;
        std::string cfg =
            "set timeout=5\n"
            "\n"
            "menuentry \"Ubuntu\" {\n"
            "    linux $(root)/vmlinuz root=/dev/sda1\n"
            "    initrd ${root}/initrd.img\n"
            "}\n"
            "\n";
        std::string p = (td.path / "grub.cfg").string();
        std::ofstream f(p);
        f << cfg;
        f.close();
        Reporter rep;
        EspContext ctx(td.path.string(), rep);
        auto parsed = parse_grub(p, ctx, rep);
        auto entries = flatten_entries(parsed.roots);
        require(entries.size() == 1, "1 entry");
        require(entries[0].first->type == "linux", "type=linux");
        require(entries[0].first->vmlinuz == "/vmlinuz",
                "vmlinuz=$root cleaned");
        require(entries[0].first->initrd == "/initrd.img",
                "initrd=${root} cleaned");
    });

    // -- 16b. GRUB submenu nesting -------------------------------------------
    check("grub-submenu-nesting", [&] {
        TempDir td;
        std::string cfg =
            "set timeout=10\n"
            "\n"
            "submenu \"Advanced\" {\n"
            "    menuentry \"Kernel 6.8\" {\n"
            "        linux /boot/vmlinuz-6.8\n"
            "    }\n"
            "    menuentry \"Kernel 6.7\" {\n"
            "        linux /boot/vmlinuz-6.7\n"
            "    }\n"
            "}\n"
            "\n"
            "menuentry \"Normal\" {\n"
            "    linux /boot/vmlinuz\n"
            "}\n"
            "\n";
        std::string p = (td.path / "grub.cfg").string();
        std::ofstream f(p);
        f << cfg;
        f.close();
        Reporter rep;
        EspContext ctx(td.path.string(), rep);
        auto parsed = parse_grub(p, ctx, rep);
        auto entries = flatten_entries(parsed.roots);
        require(entries.size() == 3, "3 entries (2 in submenu + 1 top)");
        require(parsed.roots[0].is_group(), "root0 is group (submenu)");
        require(parsed.roots[0].group->title == "Advanced", "submenu title");
        require(parsed.roots[0].group->children.size() == 2,
                "submenu has 2 children");
        require(parsed.roots[1].is_group() == false,
                "root1 is entry (Normal)");
        require(parsed.roots[1].entry->title == "Normal", "root1 title");
        require(parsed.timeout.has_value(), "timeout set");
        require(*parsed.timeout == 10, "timeout=10");
    });

    // -- 16c. GRUB linuxefi/initrdefi not matched --------------------------
    check("grub-linuxefi-not-matched", [&] {
        TempDir td;
        std::string cfg =
            "menuentry \"EFI Linux\" {\n"
            "    linuxefi /boot/vmlinuz\n"
            "    initrdefi /boot/initrd.img\n"
            "}\n"
            "\n";
        std::string p = (td.path / "grub.cfg").string();
        std::ofstream f(p);
        f << cfg;
        f.close();
        Reporter rep;
        EspContext ctx(td.path.string(), rep);
        auto parsed = parse_grub(p, ctx, rep);
        auto entries = flatten_entries(parsed.roots);
        // linuxefi/initrdefi are not recognized -> entry skipped
        require(entries.size() == 0, "linuxefi skipped -> 0 entries");
        bool warned = false;
        for (auto& w : rep.notes)
            if (w.find("no boot directive") != std::string::npos) warned = true;
        require(warned, "entry skipped warning");
    });

    // -- 16d. GRUB set default by title -------------------------------------
    check("grub-set-default-title", [&] {
        TempDir td;
        std::string cfg =
            "set default=\"Fedora>GNOME>Workstation\"\n"
            "\n"
            "menuentry \"Fedora\" {\n"
            "    submenu \"GNOME\" {\n"
            "        menuentry \"Workstation\" {\n"
            "            linux /boot/vmlinuz\n"
            "        }\n"
            "        menuentry \"Server\" {\n"
            "            linux /boot/vmlinuz\n"
            "        }\n"
            "    }\n"
            "}\n"
            "\n";
        std::string p = (td.path / "grub.cfg").string();
        std::ofstream f(p);
        f << cfg;
        f.close();
        Reporter rep;
        EspContext ctx(td.path.string(), rep);
        auto parsed = parse_grub(p, ctx, rep);
        require(parsed.def.kind == DefaultSpec::Path,
                "default set by title path");
        require(parsed.def.str == "Fedora/GNOME/Workstation",
                "default path resolved");
    });

    // -- 17. Lint: valid config (no errors) ----------------------------------
    check("lint-valid-config", [&] {
        std::string text =
            "timeout=3\n"
            "default=0\n"
            "\n"
            "menuentry \"Linux\" {\n"
            "    type=linux\n"
            "    vmlinuz=/vmlinuz\n"
            "    initrd=/initrd.img\n"
            "    cmdline=\"root=/dev/sda1\"\n"
            "    icon=tux\n"
            "}\n"
            "\n"
            "menuentry \"Windows\" {\n"
            "    type=chainload\n"
            "    chain=/EFI/Microsoft/Boot/bootmgfw.efi\n"
            "    icon=windows\n"
            "}\n";
        LintResult lr = validate_config(text);
        require(!lr.has_errors(), "no errors on valid config");
        require(!lr.has_warnings(), "no warnings on valid config");
    });

    // -- 17b. Lint: title too long ------------------------------------------
    check("lint-title-too-long", [&] {
        std::string long_title(MAX_TITLE + 10, 'A');
        std::string text =
            "menuentry \"" + long_title + "\" {\n"
            "    type=linux\n"
            "    vmlinuz=/vmlinuz\n"
            "    icon=tux\n"
            "}\n";
        LintResult lr = validate_config(text);
        require(lr.has_warnings(), "warnings for long title");
    });

    // -- 17c. Lint: cmdline too long ----------------------------------------
    check("lint-cmdline-too-long", [&] {
        std::string long_cmd(MAX_CMDLINE + 20, 'x');
        std::string text =
            "menuentry \"Linux\" {\n"
            "    type=linux\n"
            "    vmlinuz=/vmlinuz\n"
            "    cmdline=\"" + long_cmd + "\"\n"
            "    icon=tux\n"
            "}\n";
        LintResult lr = validate_config(text);
        require(lr.has_warnings(), "warnings for long cmdline");
    });

    // -- 17d. Lint: unbalanced braces (error) -------------------------------
    check("lint-unbalanced-braces", [&] {
        std::string text =
            "menuentry \"Linux\" {\n"
            "    type=linux\n"
            "    vmlinuz=/vmlinuz\n"
            "    icon=tux\n";
        LintResult lr = validate_config(text);
        require(lr.has_errors(), "error for unclosed brace");
    });

    // -- 17e. Lint: duplicate entry titles ----------------------------------
    check("lint-duplicate-titles", [&] {
        std::string text =
            "menuentry \"Linux\" {\n"
            "    type=linux\n"
            "    vmlinuz=/vmlinuz-1\n"
            "    icon=tux\n"
            "}\n"
            "\n"
            "menuentry \"Linux\" {\n"
            "    type=linux\n"
            "    vmlinuz=/vmlinuz-2\n"
            "    icon=tux\n"
            "}\n";
        ParsedConfig parsed;
        parsed.kind = "limine";
        parsed.roots.push_back(make_entry(
            []() {
                auto e = std::make_shared<OutEntry>();
                e->title = "Linux";
                e->type = "linux";
                e->vmlinuz = "/vmlinuz-1";
                e->icon = "tux";
                return e;
            }()));
        parsed.roots.push_back(make_entry(
            []() {
                auto e = std::make_shared<OutEntry>();
                e->title = "Linux";
                e->type = "linux";
                e->vmlinuz = "/vmlinuz-2";
                e->icon = "tux";
                return e;
            }()));
        LintResult lr = validate_config(parsed);
        require(lr.has_warnings(), "duplicate title warning");
        bool found_dup = false;
        for (auto& m : lr.messages)
            if (m.text.find("duplicate") != std::string::npos) found_dup = true;
        require(found_dup, "duplicate warning present");
    });

    // -- 17f. Lint: invalid entry type --------------------------------------
    check("lint-invalid-type", [&] {
        ParsedConfig parsed;
        parsed.kind = "limine";
        parsed.roots.push_back(make_entry(
            []() {
                auto e = std::make_shared<OutEntry>();
                e->title = "Bad";
                e->type = "invalid_type";
                e->vmlinuz = "/vmlinuz";
                e->icon = "tux";
                return e;
            }()));
        LintResult lr = validate_config(parsed);
        require(lr.has_warnings(), "invalid type warning");
        bool found = false;
        for (auto& m : lr.messages)
            if (m.text.find("unknown type") != std::string::npos) found = true;
        require(found, "unknown type warning present");
    });

    // -- 17g. Lint: path existence on fake ESP ------------------------------
    check("lint-path-existence", [&] {
        TempDir td;
        // Create a file that exists
        std::ofstream((td.path / "vmlinuz").string()) << "kernel";
        std::string text =
            "menuentry \"Linux\" {\n"
            "    type=linux\n"
            "    vmlinuz=/vmlinuz\n"
            "    initrd=/nonexistent-initrd\n"
            "    icon=tux\n"
            "}\n";
        LintResult lr = validate_config(text, td.path.string());
        require(lr.has_warnings(), "warnings for missing file");
        bool found_warn = false;
        for (auto& m : lr.messages)
            if (m.text.find("not found on ESP") != std::string::npos)
                found_warn = true;
        require(found_warn, "missing file warning present");
    });

    // -- 18. Uninstall dry-run -----------------------------------------------
    check("uninstall-dry-run", [&] {
        TempDir td;
        // Create fake ForeB installation
        fs::create_directories(td.path / "EFI" / "forb");
        fs::create_directories(td.path / "forebo");
        {
            std::ofstream f((td.path / "EFI" / "forb" / "BOOTX64.EFI").string());
            f << "efi-binary";
        }
        {
            std::ofstream f((td.path / "forebo" / "forebo.cfg").string());
            f << "timeout=3\n";
        }
        Args a;
        a.esp = td.path.string();
        a.dry_run = true;
        a.yes = true;
        a.no_nvram = true;
        Reporter rep(false);
        int rc = cmd_uninstall(a, rep);
        require(rc == 0, "uninstall dry-run returns 0");
        // Files should still exist (dry-run)
        require(fs::is_directory(td.path / "EFI" / "forb"),
                "EFI/forb still exists after dry-run");
        require(fs::is_regular_file(td.path / "forebo" / "forebo.cfg"),
                "forebo.cfg still exists after dry-run");
    });

    // -- 18b. Uninstall no ForeB found -------------------------------------
    check("uninstall-nothing", [&] {
        TempDir td;
        Args a;
        a.esp = td.path.string();
        a.dry_run = true;
        a.yes = true;
        a.no_nvram = true;
        Reporter rep(false);
        int rc = cmd_uninstall(a, rep);
        require(rc == 0, "uninstall nothing returns 0");
    });

    // -- 19. Export: GRUB format ---------------------------------------------
    check("export-grub", [&] {
        ParsedConfig parsed;
        parsed.kind = "limine";
        parsed.timeout = 5;
        parsed.roots.push_back(make_entry(
            []() {
                auto e = std::make_shared<OutEntry>();
                e->title = "Ubuntu";
                e->type = "linux";
                e->vmlinuz = "/vmlinuz";
                e->initrd = "/initrd.img";
                e->cmdline = "root=/dev/sda1";
                e->icon = "ubuntu";
                return e;
            }()));
        parsed.roots.push_back(make_entry(
            []() {
                auto e = std::make_shared<OutEntry>();
                e->title = "Windows";
                e->type = "chainload";
                e->chain = "/EFI/Microsoft/Boot/bootmgfw.efi";
                e->icon = "windows";
                return e;
            }()));
        parsed.index_entries = parsed.roots[0].entry
            ? std::vector<std::shared_ptr<OutEntry>>{
                parsed.roots[0].entry, parsed.roots[1].entry}
            : std::vector<std::shared_ptr<OutEntry>>{};
        std::string grub = export_to_grub(parsed, parsed.roots);
        require(grub.find("set timeout=5") != std::string::npos,
                "timeout in grub");
        require(grub.find("menuentry \"Ubuntu\"") != std::string::npos,
                "Ubuntu menuentry");
        require(grub.find("linux /vmlinuz") != std::string::npos,
                "linux directive");
        require(grub.find("initrd /initrd.img") != std::string::npos,
                "initrd directive");
        require(grub.find("chainloader /EFI/Microsoft/Boot/bootmgfw.efi") !=
                std::string::npos, "chainloader directive");
        require(grub.find("root=/dev/sda1") != std::string::npos,
                "cmdline in linux line");
    });

    // -- 19b. Export: Limine format -----------------------------------------
    check("export-limine", [&] {
        ParsedConfig parsed;
        parsed.kind = "grub";
        parsed.timeout = 10;
        parsed.roots.push_back(make_entry(
            []() {
                auto e = std::make_shared<OutEntry>();
                e->title = "Arch";
                e->type = "linux";
                e->vmlinuz = "/vmlinuz";
                e->initrd = "/initrd.img";
                e->cmdline = "root=/dev/sdb2";
                e->icon = "arch";
                return e;
            }()));
        parsed.index_entries = std::vector<std::shared_ptr<OutEntry>>{
            parsed.roots[0].entry};
        std::string lim = export_to_limine(parsed, parsed.roots);
        require(lim.find("TIMEOUT=10") != std::string::npos, "TIMEOUT");
        require(lim.find(":Arch") != std::string::npos, "entry title");
        require(lim.find("PROTOCOL=linux") != std::string::npos,
                "PROTOCOL=linux");
        require(lim.find("KERNEL_PATH=boot():/") != std::string::npos,
                "KERNEL_PATH");
        require(lim.find("CMDLINE=root=/dev/sdb2") != std::string::npos,
                "CMDLINE");
        require(lim.find("MODULE_PATH=boot():/initrd.img") !=
                std::string::npos, "MODULE_PATH for initrd");
    });

    // -- 19c. Export: systemd-boot format -----------------------------------
    check("export-systemd", [&] {
        ParsedConfig parsed;
        parsed.kind = "limine";
        parsed.timeout = 7;
        parsed.roots.push_back(make_entry(
            []() {
                auto e = std::make_shared<OutEntry>();
                e->title = "Fedora";
                e->type = "linux";
                e->vmlinuz = "/vmlinuz-fedora";
                e->initrd = "/initrd-fedora";
                e->cmdline = "root=LABEL=FEDORA";
                e->icon = "fedora";
                return e;
            }()));
        parsed.index_entries = std::vector<std::shared_ptr<OutEntry>>{
            parsed.roots[0].entry};
        std::string sd = export_to_systemd(parsed, parsed.roots);
        require(sd.find("timeout 7") != std::string::npos, "timeout");
        require(sd.find("title Fedora") != std::string::npos, "title");
        require(sd.find("linux /vmlinuz-fedora") != std::string::npos,
                "linux line");
        require(sd.find("initrd /initrd-fedora") != std::string::npos,
                "initrd line");
        require(sd.find("options root=LABEL=FEDORA") != std::string::npos,
                "options line");
    });

    // -- 19d. Export: syslinux format ---------------------------------------
    check("export-syslinux", [&] {
        ParsedConfig parsed;
        parsed.kind = "limine";
        parsed.timeout = 4;
        parsed.roots.push_back(make_entry(
            []() {
                auto e = std::make_shared<OutEntry>();
                e->title = "Gentoo";
                e->type = "linux";
                e->vmlinuz = "/boot/vmlinuz-gentoo";
                e->initrd = "/boot/initrd-gentoo";
                e->cmdline = "root=/dev/sda3";
                e->icon = "tux";
                return e;
            }()));
        parsed.index_entries = std::vector<std::shared_ptr<OutEntry>>{
            parsed.roots[0].entry};
        std::string sl = export_to_syslinux(parsed, parsed.roots);
        require(sl.find("TIMEOUT 40") != std::string::npos,
                "TIMEOUT=4*10=40");
        require(sl.find("DEFAULT") != std::string::npos, "DEFAULT label");
        require(sl.find("LINUX /boot/vmlinuz-gentoo") != std::string::npos,
                "LINUX directive");
        require(sl.find("APPEND root=/dev/sda3") != std::string::npos,
                "APPEND directive");
        require(sl.find("INITRD /boot/initrd-gentoo") != std::string::npos,
                "INITRD directive");
    });

    // -- 19e. Export round-trip: parse -> export -> re-parse ----------------
    check("export-roundtrip-grub", [&] {
        TempDir td;
        // Parse a grub config
        std::string grub_cfg =
            "set timeout=5\n"
            "\n"
            "menuentry \"Ubuntu\" {\n"
            "    linux /vmlinuz root=/dev/sda1\n"
            "    initrd /initrd.img\n"
            "}\n"
            "\n"
            "menuentry \"Windows\" {\n"
            "    chainloader /EFI/Microsoft/Boot/bootmgfw.efi\n"
            "}\n"
            "\n";
        std::string p = (td.path / "grub.cfg").string();
        {
            std::ofstream f(p);
            f << grub_cfg;
        }
        Reporter rep;
        EspContext ctx(td.path.string(), rep);
        auto parsed = parse_grub(p, ctx, rep);
        auto entries = flatten_entries(parsed.roots);
        require(entries.size() == 2, "parsed 2 entries");

        // Export to grub
        std::string exported = export_to_grub(parsed, parsed.roots);
        require(exported.find("menuentry \"Ubuntu\"") != std::string::npos,
                "re-exported Ubuntu");
        require(exported.find("menuentry \"Windows\"") != std::string::npos,
                "re-exported Windows");
        require(exported.find("set timeout=") != std::string::npos,
                "timeout preserved");
    });

    // -- 20. Migration: detect_bootloader() ----------------------------------
    check("detect-bootloader-grub", [&] {
        TempDir td;
        fs::create_directories(td.path / "grub");
        std::ofstream((td.path / "grub" / "grub.cfg").string())
            << "set timeout=5\nmenuentry \"Linux\" { linux /vmlinuz }\n";
        auto results = detect_bootloader(td.path.string());
        require(!results.empty(), "grub detected");
        require(results[0].name == "grub", "name=grub");
        require(results[0].confidence >= 0.8, "high confidence");
    });

    check("detect-bootloader-limine", [&] {
        TempDir td;
        std::ofstream((td.path / "limine.conf").string())
            << "/+Linux\n    protocol: linux\n    path: boot():/vmlinuz\n";
        auto results = detect_bootloader(td.path.string());
        require(!results.empty(), "limine detected");
        bool found_limine = false;
        for (auto& r : results)
            if (r.name == "limine") found_limine = true;
        require(found_limine, "limine in results");
    });

    check("detect-bootloader-systemd-boot", [&] {
        TempDir td;
        fs::create_directories(td.path / "loader" / "entries");
        std::ofstream((td.path / "loader" / "loader.conf").string())
            << "timeout 3\n";
        std::ofstream((td.path / "loader" / "entries" / "arch.conf").string())
            << "title Arch\nlinux /vmlinuz\n";
        auto results = detect_bootloader(td.path.string());
        require(!results.empty(), "systemd-boot detected");
        bool found_sdb = false;
        for (auto& r : results)
            if (r.name == "systemd-boot") found_sdb = true;
        require(found_sdb, "systemd-boot in results");
    });

    check("detect-bootloader-refind", [&] {
        TempDir td;
        fs::create_directories(td.path / "EFI" / "refind");
        std::ofstream((td.path / "EFI" / "refind" / "refind.conf").string())
            << "menuentry \"Linux\" {\n    loader /EFI/vmlinuz\n}\n";
        auto results = detect_bootloader(td.path.string());
        require(!results.empty(), "refind detected");
        bool found = false;
        for (auto& r : results)
            if (r.name == "refind") found = true;
        require(found, "refind in results");
    });

    check("detect-bootloader-clover", [&] {
        TempDir td;
        fs::create_directories(td.path / "EFI" / "CLOVER");
        std::ofstream((td.path / "EFI" / "CLOVER" / "config.plist").string())
            << "<plist><dict></dict></plist>\n";
        auto results = detect_bootloader(td.path.string());
        require(!results.empty(), "clover detected");
        bool found = false;
        for (auto& r : results)
            if (r.name == "clover") found = true;
        require(found, "clover in results");
    });

    // -- 21. Validation: validate_entry() ------------------------------------
    check("validate-entry-valid", [&] {
        Reporter rep;
        OutEntry e;
        e.title = "Short";
        e.type = "linux";
        e.vmlinuz = "/vmlinuz";
        e.initrd = "/initrd.img";
        e.cmdline = "root=/dev/sda1";
        e.icon = "tux";
        validate_entry(e, rep);
        require(rep.warnings.empty(), "no warnings for valid entry");
        require(e.title == "Short", "title unchanged");
    });

    check("validate-entry-title-too-long", [&] {
        Reporter rep;
        OutEntry e;
        e.title = std::string(MAX_TITLE + 15, 'X');
        e.type = "linux";
        e.vmlinuz = "/vmlinuz";
        e.icon = "tux";
        validate_entry(e, rep);
        require(!rep.warnings.empty(), "warnings for long title");
        require(static_cast<int>(e.title.size()) <= MAX_TITLE,
                "title truncated");
    });

    check("validate-entry-cmdline-quote-replace", [&] {
        Reporter rep;
        OutEntry e;
        e.title = "Test";
        e.type = "linux";
        e.vmlinuz = "/vmlinuz";
        e.cmdline = R"(root=/dev/sda1 "quoted")";
        e.icon = "tux";
        validate_entry(e, rep);
        require(e.cmdline.find('"') == std::string::npos,
                "double quotes replaced");
        require(e.cmdline.find("'quoted'") != std::string::npos,
                "replaced with single quotes");
    });

    // -- 22. Backup ----------------------------------------------------------
    check("backup-dry-run", [&] {
        TempDir td;
        // Create fake ForeB install
        fs::create_directories(td.path / "EFI" / "forb");
        fs::create_directories(td.path / "forebo");
        std::ofstream((td.path / "forebo" / "forebo.cfg").string())
            << "timeout=3\n";
        std::string outpath = (td.path / "test-backup.tar.gz").string();
        Args a;
        a.esp = td.path.string();
        a.output = outpath;
        a.dry_run = false;
        Reporter rep(false);
        // Run actual backup (creates tar.gz)
        int rc = cmd_backup(a, rep);
        require(rc == 0, "backup returns 0");
        require(fs::is_regular_file(outpath), "tar.gz created");
        // Verify the archive contains our files
        auto listing = run_cmd({"tar", "tzf", outpath});
        require(listing.has_value(), "tar tzf succeeds");
        std::string contents = *listing;
        require(contents.find("forebo/forebo.cfg") != std::string::npos,
                "forebo.cfg in archive");
    });

    // -- 23. emit_config basic output ----------------------------------------
    check("emit-config-basic", [&] {
        ParsedConfig parsed;
        parsed.kind = "limine";
        parsed.source_path = "test.conf";
        parsed.timeout = 3;
        parsed.remember_last = true;
        parsed.roots.push_back(make_entry(
            []() {
                auto e = std::make_shared<OutEntry>();
                e->title = "Test Linux";
                e->type = "linux";
                e->vmlinuz = "/vmlinuz";
                e->initrd = "/initrd.img";
                e->cmdline = "quiet";
                e->icon = "tux";
                return e;
            }()));
        parsed.index_entries = std::vector<std::shared_ptr<OutEntry>>{
            parsed.roots[0].entry};
        std::string cfg = emit_config(parsed, parsed.roots, "Test Linux",
                                       std::nullopt, false);
        require(cfg.find("timeout=3") != std::string::npos, "timeout line");
        require(cfg.find("default=Test Linux") != std::string::npos,
                "default line");
        require(cfg.find("remember_last=1") != std::string::npos,
                "remember_last");
        require(cfg.find("menuentry \"Test Linux\"") != std::string::npos,
                "menuentry");
        require(cfg.find("type=linux") != std::string::npos, "type=linux");
        require(cfg.find("vmlinuz=/vmlinuz") != std::string::npos,
                "vmlinuz line");
        require(cfg.find("initrd=/initrd.img") != std::string::npos,
                "initrd line");
        require(cfg.find("cmdline=\"quiet\"") != std::string::npos,
                "cmdline line");
    });

    // -- 24. emit_config with submenu ----------------------------------------
    check("emit-config-submenu", [&] {
        ParsedConfig parsed;
        parsed.kind = "limine";
        parsed.source_path = "test.conf";
        parsed.timeout = 5;
        auto sub = std::make_shared<OutGroup>();
        sub->title = "Advanced";
        sub->icon = "gear";
        sub->children.push_back(make_entry(
            []() {
                auto e = std::make_shared<OutEntry>();
                e->title = "Kernel 6.8";
                e->type = "linux";
                e->vmlinuz = "/vmlinuz-6.8";
                e->initrd = "/initrd-6.8";
                e->icon = "tux";
                return e;
            }()));
        parsed.roots.push_back(make_group(sub));
        parsed.roots.push_back(make_entry(
            []() {
                auto e = std::make_shared<OutEntry>();
                e->title = "Normal";
                e->type = "linux";
                e->vmlinuz = "/vmlinuz";
                e->initrd = "/initrd.img";
                e->icon = "tux";
                return e;
            }()));
        auto flat = flatten_entries(parsed.roots);
        for (auto& [e, p] : flat) parsed.index_entries.push_back(e);
        std::string cfg = emit_config(parsed, parsed.roots, "Normal",
                                       std::nullopt, false);
        require(cfg.find("submenu \"Advanced\"") != std::string::npos,
                "submenu header");
        require(cfg.find("icon=gear") != std::string::npos, "group icon");
        require(cfg.find("menuentry \"Kernel 6.8\"") != std::string::npos,
                "submenu entry");
        require(cfg.find("menuentry \"Normal\"") != std::string::npos,
                "top entry");
    });

    // -- 25. flatten + find_path ---------------------------------------------
    check("find-path-in-tree", [&] {
        OutNode root;
        auto g = std::make_shared<OutGroup>();
        g->title = "Sub";
        auto e1 = std::make_shared<OutEntry>();
        e1->title = "Child";
        e1->type = "linux";
        g->children.push_back(make_entry(e1));
        root = make_group(g);
        std::vector<OutNode> roots = {root};
        auto path = find_path(roots, e1);
        require(path.has_value(), "find_path succeeded");
        require(path->size() == 2, "path depth = 2");
        require((*path)[0] == "Sub", "parent group");
        require((*path)[1] == "Child", "entry title");
    });

    // -- 26. cap_entries -----------------------------------------------------
    check("cap-entries", [&] {
        std::vector<OutNode> roots;
        for (int i = 0; i < 5; ++i) {
            auto e = std::make_shared<OutEntry>();
            e->title = "Entry " + std::to_string(i);
            e->type = "linux";
            e->vmlinuz = "/vmlinuz";
            e->icon = "tux";
            roots.push_back(make_entry(e));
        }
        Reporter rep;
        auto capped = cap_entries(std::move(roots), 3, rep);
        auto flat = flatten_entries(capped);
        require(flat.size() == 3, "capped to 3 entries");
        bool warned = false;
        for (auto& w : rep.warnings)
            if (w.find("--max-entries") != std::string::npos) warned = true;
        require(warned, "cap warning emitted");
    });

    // -- 27. resolve_default -------------------------------------------------
    check("resolve-default-index", [&] {
        ParsedConfig parsed;
        parsed.def.kind = DefaultSpec::Index;
        parsed.def.index = 2;
        std::vector<OutNode> roots;
        for (int i = 0; i < 3; ++i) {
            auto e = std::make_shared<OutEntry>();
            e->title = "E" + std::to_string(i);
            e->type = "linux";
            e->vmlinuz = "/vmlinuz";
            e->icon = "tux";
            parsed.index_entries.push_back(e);
            roots.push_back(make_entry(e));
        }
        Reporter rep;
        std::string def = resolve_default(parsed, roots, std::nullopt, rep);
        require(def == "E1", "resolved to E1 (index 2)");
    });

    check("resolve-default-override", [&] {
        ParsedConfig parsed;
        parsed.def.kind = DefaultSpec::None;
        std::vector<OutNode> roots;
        auto e = std::make_shared<OutEntry>();
        e->title = "Only";
        e->type = "linux";
        e->vmlinuz = "/vmlinuz";
        e->icon = "tux";
        parsed.index_entries.push_back(e);
        roots.push_back(make_entry(e));
        Reporter rep;
        std::string def = resolve_default(parsed, roots, 1, rep);
        require(def == "Only", "override index=1 -> Only");
    });

    // -- 28. validate_config: empty config ----------------------------------
    check("lint-empty-config", [&] {
        LintResult lr = validate_config("");
        require(lr.has_errors(), "error for empty config");
        require(lr.errors() == 1, "exactly 1 error");
    });

    // -- 29. clover.conf INI format -----------------------------------------
    check("clover-conf-ini", [&] {
        TempDir td;
        std::string ini =
            "[BOOT]\n"
            "Timeout=5\n"
            "\n"
            "[System]\n"
            "Title=Arch Linux\n"
            "Type=0\n"
            "Kernel=/EFI/arch/vmlinuz\n"
            "KernelFlags=root=/dev/sda1\n"
            "\n"
            "[System]\n"
            "Title=Windows\n"
            "Type=2\n"
            "Path=\\EFI\\Microsoft\\Boot\\bootmgfw.efi\n"
            "\n";
        std::string p = (td.path / "clover.conf").string();
        std::ofstream f(p);
        f << ini;
        f.close();
        Reporter rep;
        EspContext ctx(td.path.string(), rep);
        auto parsed = parse_clover(p, ctx, rep);
        require(parsed.kind == "clover", "kind=clover");
        require(parsed.timeout.has_value(), "timeout set");
        require(*parsed.timeout == 5, "timeout=5");
        auto entries = flatten_entries(parsed.roots);
        require(entries.size() == 2, "2 entries");
        require(entries[0].first->title == "Arch Linux", "entry0 title");
        require(entries[0].first->type == "linux", "entry0 type=linux");
        require(entries[0].first->vmlinuz == "/EFI/arch/vmlinuz",
                "entry0 kernel");
        require(entries[1].first->title == "Windows", "entry1 title");
        require(entries[1].first->type == "chainload",
                "entry1 type=chainload");
    });

    // -- 30. syslinux INCLUDE ------------------------------------------------
    check("syslinux-include", [&] {
        TempDir td;
        std::ofstream((td.path / "extra.cfg").string())
            << "LABEL extra\n"
            << "    KERNEL /extra-kernel\n"
            << "    APPEND extra-param\n";
        std::string main_cfg =
            "INCLUDE " + (td.path / "extra.cfg").string() + "\n"
            "\n"
            "LABEL main\n"
            "    KERNEL /main-kernel\n";
        std::string p = (td.path / "syslinux.cfg").string();
        {
            std::ofstream f(p);
            f << main_cfg;
        }
        Reporter rep;
        EspContext ctx(td.path.string(), rep);
        auto parsed = parse_syslinux(p, ctx, rep);
        auto entries = flatten_entries(parsed.roots);
        require(entries.size() == 2, "2 entries from INCLUDE + main");
        require(entries[0].first->title == "extra", "included entry first");
        require(entries[0].first->vmlinuz == "/extra-kernel",
                "included entry kernel");
        require(entries[1].first->title == "main", "main entry second");
    });

    // -- 31. rEFInd backslash path normalization ----------------------------
    check("refind-backslash-paths", [&] {
        TempDir td;
        std::string cfg =
            "menuentry \"Windows\" {\n"
            "    loader \\EFI\\Microsoft\\Boot\\bootmgfw.efi\n"
            "    icon \\EFI\\icons\\windows.icn\n"
            "}\n"
            "\n";
        std::string p = (td.path / "refind.conf").string();
        {
            std::ofstream f(p);
            f << cfg;
        }
        Reporter rep;
        EspContext ctx(td.path.string(), rep);
        auto parsed = parse_refind(p, ctx, rep);
        auto entries = flatten_entries(parsed.roots);
        require(entries.size() == 1, "1 entry");
        require(entries[0].first->chain ==
                "/EFI/Microsoft/Boot/bootmgfw.efi",
                "backslash -> forward slash");
    });

    // -- 32. GRUB set default numeric ---------------------------------------
    check("grub-set-default-numeric", [&] {
        TempDir td;
        std::string cfg =
            "set default=1\n"
            "\n"
            "menuentry \"A\" { linux /a }\n"
            "menuentry \"B\" { linux /b }\n"
            "\n";
        std::string p = (td.path / "grub.cfg").string();
        {
            std::ofstream f(p);
            f << cfg;
        }
        Reporter rep;
        EspContext ctx(td.path.string(), rep);
        auto parsed = parse_grub(p, ctx, rep);
        require(parsed.def.kind == DefaultSpec::Index, "default is index");
        require(parsed.def.index == 2, "grub 0-based -> 1-based = 2");
    });

    // -- 33. validate_config with parsed + ESP path check -------------------
    check("validate-parsed-with-esp", [&] {
        TempDir td;
        fs::create_directories(td.path / "EFI" / "arch");
        std::ofstream((td.path / "EFI" / "arch" / "vmlinuz").string()) << "k";
        ParsedConfig parsed;
        parsed.kind = "limine";
        auto e = std::make_shared<OutEntry>();
        e->title = "Arch";
        e->type = "linux";
        e->vmlinuz = "/EFI/arch/vmlinuz";
        e->initrd = "/EFI/arch/missing-initrd";
        e->icon = "arch";
        parsed.roots.push_back(make_entry(e));
        parsed.index_entries.push_back(e);
        LintResult lr = validate_config(parsed, td.path.string());
        bool found = false;
        for (auto& m : lr.messages)
            if (m.text.find("not found on ESP") != std::string::npos)
                found = true;
        require(found, "missing initrd detected on ESP");
    });

    // -- 34. grub with chainloader + classes ---------------------------------
    check("grub-chainloader-classes", [&] {
        TempDir td;
        std::string cfg =
            "menuentry \"Windows\" --class windows {\n"
            "    chainloader /EFI/Microsoft/Boot/bootmgfw.efi\n"
            "}\n"
            "\n";
        std::string p = (td.path / "grub.cfg").string();
        {
            std::ofstream f(p);
            f << cfg;
        }
        Reporter rep;
        EspContext ctx(td.path.string(), rep);
        auto parsed = parse_grub(p, ctx, rep);
        auto entries = flatten_entries(parsed.roots);
        require(entries.size() == 1, "1 chainload entry");
        require(entries[0].first->type == "chainload", "type=chainload");
        require(entries[0].first->chain ==
                "/EFI/Microsoft/Boot/bootmgfw.efi", "chain path");
        require(entries[0].first->icon == "windows", "windows icon from class");
    });

    // -- 35. systemd-boot with efi= directive -------------------------------
    check("systemd-boot-efi-directive", [&] {
        TempDir td;
        fs::create_directories(td.path / "loader" / "entries");
        std::ofstream((td.path / "loader" / "loader.conf").string())
            << "timeout 2\n";
        std::ofstream((td.path / "loader" / "entries" / "win.conf").string())
            << "title Windows\n"
            << "efi /EFI/Microsoft/Boot/bootmgfw.efi\n";
        std::ofstream((td.path / "loader" / "entries" / "arch.conf").string())
            << "title Arch\n"
            << "linux /vmlinuz\n"
            << "initrd /initrd.img\n"
            << "options root=/dev/sda2\n";
        Reporter rep;
        auto parsed = parse_systemd_boot(td.path.string(),
            (td.path / "loader" / "loader.conf").string(), rep);
        auto entries = flatten_entries(parsed.roots);
        require(entries.size() == 2, "2 entries");
        require(entries[0].first->title == "Arch", "arch first (sorted)");
        require(entries[0].first->type == "linux", "arch type=linux");
        require(entries[1].first->title == "Windows", "win second");
        require(entries[1].first->type == "chainload", "win type=chainload");
        require(parsed.timeout.has_value(), "timeout set");
        require(*parsed.timeout == 2, "timeout=2");
    });

    // -- 36. GRUB multiboot entry -------------------------------------------
    check("grub-multiboot", [&] {
        TempDir td;
        std::string cfg =
            "menuentry \"ForeB OS\" {\n"
            "    multiboot /boot/foreb.elf\n"
            "    module /boot/data.bin\n"
            "    module /boot/config.dat\n"
            "}\n"
            "\n";
        std::string p = (td.path / "grub.cfg").string();
        {
            std::ofstream f(p);
            f << cfg;
        }
        Reporter rep;
        EspContext ctx(td.path.string(), rep);
        auto parsed = parse_grub(p, ctx, rep);
        auto entries = flatten_entries(parsed.roots);
        require(entries.size() == 1, "1 multiboot entry");
        require(entries[0].first->type == "forest", "type=forest");
        require(entries[0].first->kernel == "/boot/foreb.elf",
                "multiboot kernel");
        require(entries[0].first->modules.size() == 2, "2 modules");
        require(entries[0].first->modules[0] == "/boot/data.bin",
                "module 0");
        require(entries[0].first->modules[1] == "/boot/config.dat",
                "module 1");
    });

    // -- 37. export chainload + forest types --------------------------------
    check("export-forest-type", [&] {
        ParsedConfig parsed;
        parsed.kind = "limine";
        parsed.timeout = 3;
        auto e = std::make_shared<OutEntry>();
        e->title = "ForeB";
        e->type = "forest";
        e->kernel = "/boot/foreb.elf";
        e->modules = {"/boot/mod1.bin"};
        e->icon = "os";
        parsed.roots.push_back(make_entry(e));
        parsed.index_entries.push_back(e);

        std::string grub = export_to_grub(parsed, parsed.roots);
        require(grub.find("linux /boot/foreb.elf") != std::string::npos,
                "forest->linux in grub export");
        require(grub.find("module /boot/mod1.bin") != std::string::npos,
                "module in grub export");

        std::string sl = export_to_syslinux(parsed, parsed.roots);
        require(sl.find("LINUX /boot/foreb.elf") != std::string::npos,
                "forest->LINUX in syslinux export");
    });

    // -- 38. detect_bootloader: nothing found --------------------------------
    check("detect-bootloader-empty", [&] {
        TempDir td;
        auto results = detect_bootloader(td.path.string());
        require(results.empty(), "empty dir -> no bootloaders");
    });

    // -- 39. cap_entries with submenu pruning --------------------------------
    check("cap-entries-submenu-pruned", [&] {
        std::vector<OutNode> roots;
        auto g = std::make_shared<OutGroup>();
        g->title = "Sub";
        g->children.push_back(
            make_entry([]() {
                auto e = std::make_shared<OutEntry>();
                e->title = "InSub";
                e->type = "linux";
                e->vmlinuz = "/vmlinuz";
                e->icon = "tux";
                return e;
            }()));
        roots.push_back(make_group(g));
        roots.push_back(
            make_entry([]() {
                auto e = std::make_shared<OutEntry>();
                e->title = "Top";
                e->type = "linux";
                e->vmlinuz = "/vmlinuz";
                e->icon = "tux";
                return e;
            }()));
        Reporter rep;
        auto capped = cap_entries(std::move(roots), 1, rep);
        auto flat = flatten_entries(capped);
        require(flat.size() == 1, "capped to 1 entry");
        require(flat[0].first->title == "InSub", "kept submenu entry first");
    });

    // -- 40. emit_config chainload -------------------------------------------
    check("emit-config-chainload", [&] {
        ParsedConfig parsed;
        parsed.kind = "limine";
        parsed.source_path = "test.conf";
        auto e = std::make_shared<OutEntry>();
        e->title = "Windows";
        e->type = "chainload";
        e->chain = "/EFI/Microsoft/Boot/bootmgfw.efi";
        e->icon = "windows";
        parsed.roots.push_back(make_entry(e));
        parsed.index_entries.push_back(e);
        std::string cfg = emit_config(parsed, parsed.roots, "Windows",
                                       std::nullopt, false);
        require(cfg.find("chain=/EFI/Microsoft/Boot/bootmgfw.efi") !=
                std::string::npos, "chain path in output");
        require(cfg.find("icon=windows") != std::string::npos,
                "windows icon in output");
    });

    // -- 41. grub default index out of range --------------------------------
    check("grub-default-out-of-range", [&] {
        TempDir td;
        std::string cfg =
            "set default=99\n"
            "\n"
            "menuentry \"A\" { linux /a }\n"
            "\n";
        std::string p = (td.path / "grub.cfg").string();
        {
            std::ofstream f(p);
            f << cfg;
        }
        Reporter rep;
        EspContext ctx(td.path.string(), rep);
        auto parsed = parse_grub(p, ctx, rep);
        std::vector<OutNode> roots = parsed.roots;
        std::string def = resolve_default(parsed, roots, std::nullopt, rep);
        require(def == "0", "out of range -> default=0");
    });

    // -- 42. syslinux APPEND without INITRD directive -----------------------
    check("syslinux-append-initrd", [&] {
        TempDir td;
        std::string cfg =
            "LABEL test\n"
            "    KERNEL /vmlinuz\n"
            "    APPEND root=/dev/sda1 initrd=/my-initrd.img\n";
        std::string p = (td.path / "syslinux.cfg").string();
        {
            std::ofstream f(p);
            f << cfg;
        }
        Reporter rep;
        EspContext ctx(td.path.string(), rep);
        auto parsed = parse_syslinux(p, ctx, rep);
        auto entries = flatten_entries(parsed.roots);
        require(entries.size() == 1, "1 entry");
        require(entries[0].first->cmdline == "root=/dev/sda1",
                "initrd= stripped from cmdline");
        require(entries[0].first->initrd == "/my-initrd.img",
                "initrd extracted from APPEND");
    });

    // -- 43. rEFInd with ostype ---------------------------------------------
    check("refind-ostype", [&] {
        TempDir td;
        std::string cfg =
            "menuentry \"Linux\" {\n"
            "    loader /vmlinuz\n"
            "    ostype Linux\n"
            "}\n"
            "\n";
        std::string p = (td.path / "refind.conf").string();
        {
            std::ofstream f(p);
            f << cfg;
        }
        Reporter rep;
        EspContext ctx(td.path.string(), rep);
        auto parsed = parse_refind(p, ctx, rep);
        auto entries = flatten_entries(parsed.roots);
        require(entries.size() == 1, "1 entry");
        require(entries[0].first->type == "linux", "ostype=Linux -> linux");
    });

    // -- 44. find_path for top-level entry ----------------------------------
    check("find-path-top-level", [&] {
        auto e = std::make_shared<OutEntry>();
        e->title = "Solo";
        e->type = "linux";
        std::vector<OutNode> roots = {make_entry(e)};
        auto path = find_path(roots, e);
        require(path.has_value(), "found");
        require(path->size() == 1, "single-element path");
        require((*path)[0] == "Solo", "path = [Solo]");
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
