// commands.cpp - build pipeline + scan/generate/install/lint commands.
#include "forb/forb.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>

#include <filesystem>
#include <unistd.h>

namespace fs = std::filesystem;

namespace forb {

// ===========================================================================
//  Build pipeline
// ===========================================================================
std::vector<std::pair<std::string, std::string>> autodetect_config(
    const std::string& esp) {
    std::vector<std::pair<std::string, std::string>> cands = {
        {"limine", (fs::path(esp) / "limine.conf").string()},
        {"limine", (fs::path(esp) / "limine.cfg").string()},
        {"limine", (fs::path(esp) / "limine" / "limine.conf").string()},
        {"limine", (fs::path(esp) / "limine" / "limine.cfg").string()},
        {"grub", (fs::path(esp) / "grub" / "grub.cfg").string()},
        {"systemd-boot", (fs::path(esp) / "loader" / "loader.conf").string()},
        {"syslinux", (fs::path(esp) / "boot" / "syslinux" / "syslinux.cfg").string()},
        {"syslinux", (fs::path(esp) / "boot" / "isolinux" / "isolinux.cfg").string()},
        {"syslinux", (fs::path(esp) / "syslinux" / "syslinux.cfg").string()},
        {"refind", (fs::path(esp) / "EFI" / "refind" / "refind.conf").string()},
        {"refind", (fs::path(esp) / "boot" / "refind.conf").string()},
        {"refind", (fs::path(esp) / "efi" / "refind" / "refind.conf").string()},
        {"zfsbootmenu", "/etc/default/zfsbootmenu"},
        {"clover", (fs::path(esp) / "EFI" / "CLOVER" / "config.plist").string()},
        {"clover", (fs::path(esp) / "EFI" / "clover" / "config.plist").string()},
        {"clover", (fs::path(esp) / "boot" / "efi" / "EFI" / "CLOVER" / "config.plist").string()},
        {"clover", (fs::path(esp) / "boot" / "efi" / "EFI" / "clover" / "config.plist").string()},
        {"clover", (fs::path(esp) / "boot" / "efi" / "EFI" / "clover" / "clover.conf").string()},
    };
    std::vector<std::pair<std::string, std::string>> found;
    for (auto& kp : cands) {
        std::error_code ec;
        if (fs::is_regular_file(kp.second, ec)) found.push_back(kp);
    }
    return found;
}

std::string infer_kind(const std::string& path) {
    std::string base = lower(base_name(path));
    if (base.find("limine") != std::string::npos) return "limine";
    if (base.find("grub") != std::string::npos) return "grub";
    if (base == "loader.conf") return "systemd-boot";
    if (base == "syslinux.cfg" || base == "isolinux.cfg") return "syslinux";
    if (base == "refind.conf") return "refind";
    if (base == "config.plist") {
        // Check if it looks like a Clover plist.
        std::string head;
        try {
            head = read_text(path);
            if (head.size() > 4096) head = head.substr(0, 4096);
        } catch (...) {
            return "clover";
        }
        if (head.find("<key>GUI</key>") != std::string::npos)
            return "clover";
        // Default to clover for .plist files.
        return "clover";
    }
    if (base == "clover.conf") return "clover";
    std::string head;
    try {
        head = read_text(path);
        if (head.size() > 8192) head = head.substr(0, 8192);
    } catch (...) {
        return "limine";
    }
    static const std::regex slash_re(R"(^\s*/+)",
                                      std::regex::multiline);
    static const std::regex proto_re(R"(^\s*protocol\s*:)",
                                      std::regex::multiline);
    if (std::regex_search(head, slash_re) ||
        std::regex_search(head, proto_re))
        return "limine";
    if (head.find("menuentry") != std::string::npos) return "grub";
    // syslinux detection: look for LABEL or KERNEL directives
    static const std::regex label_re(R"(^\s*LABEL\b)", std::regex::multiline);
    static const std::regex kernel_re(R"(^\s*KERNEL\b)", std::regex::multiline);
    if (std::regex_search(head, label_re) ||
        std::regex_search(head, kernel_re))
        return "syslinux";
    // ZBM detection: look for ZBM_ variable assignments
    if (head.find("ZBM_") != std::string::npos) return "zfsbootmenu";
    return "limine";
}

static ParsedConfig parse_source(const std::string& kind,
                                 const std::string& path,
                                 const std::string& esp, EspContext& ctx,
                                 Reporter& rep) {
    if (kind == "limine") return parse_limine(path, ctx, rep);
    if (kind == "grub") return parse_grub(path, ctx, rep);
    if (kind == "syslinux") return parse_syslinux(path, ctx, rep);
    if (kind == "refind") return parse_refind(path, ctx, rep);
    if (kind == "zfsbootmenu") return parse_zfsbootmenu(path, ctx, rep);
    if (kind == "clover") return parse_clover(path, ctx, rep);
    if (kind == "systemd-boot") {
        std::string esp_dir = esp;
        fs::path p(path);
        if (p.filename() == "loader.conf" &&
            p.parent_path().filename() == "loader")
            esp_dir = p.parent_path().parent_path().string();
        return parse_systemd_boot(esp_dir, path, rep);
    }
    throw std::runtime_error("unknown config kind " + kind);
}

// returns (background value, optional (dest name, bytes))
static std::pair<std::optional<std::string>,
                 std::optional<std::pair<std::string, std::string>>>
prepare_wallpaper(const ParsedConfig& cfg, EspContext& ctx, Reporter& rep) {
    if (!cfg.wallpaper) return {std::nullopt, std::nullopt};
    std::string src = ctx.esp_file(*cfg.wallpaper);
    std::error_code ec;
    if (!fs::is_regular_file(src, ec)) {
        rep.warn("wallpaper '" +
                 (cfg.wallpaper_raw ? *cfg.wallpaper_raw : *cfg.wallpaper) +
                 "' (" + src + ") not found on the ESP; no background will be "
                 "set");
        return {std::nullopt, std::nullopt};
    }
    std::string ext = lower(fs::path(src).extension().string());
    std::string dest_name = base_name(*cfg.wallpaper);
    std::string data;
    try {
        std::ifstream f(src, std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        data = ss.str();
    } catch (...) {
        rep.warn("cannot read wallpaper " + src +
                 "; no background will be set");
        return {std::nullopt, std::nullopt};
    }
    if (ext == ".bmp" || ext == ".tga") {
        return {std::optional<std::string>("/forebo/" + dest_name),
                std::make_pair(dest_name, data)};
    }
    std::string bmp;
    try {
        bmp = png_to_bmp(data);
    } catch (const PngError& e) {
        rep.warn("wallpaper " + src + " cannot be converted to BMP (" +
                 e.msg + "); no background will be set");
        return {std::nullopt, std::nullopt};
    }
    rep.note("wallpaper " + src + " converted to BMP");
    return {std::optional<std::string>("/forebo/wallpaper.bmp"),
            std::make_pair(std::string("wallpaper.bmp"), bmp)};
}

std::optional<BuildResult> build_config(const Args& args, Reporter& rep) {
    std::string esp = args.esp;
    if (esp.empty()) {
        auto d = detect_esp();
        if (!d) {
            die("no ESP found: none of /boot, /boot/efi, /efi is a vfat mount "
                "(use --esp PATH)");
            return std::nullopt;
        }
        esp = *d;
        rep.note("auto-detected ESP at " + esp);
    }
    EspContext ctx(esp, rep);

    std::pair<std::string, std::string> source;
    if (!args.config.empty()) {
        std::string kind = infer_kind(args.config);
        source = {kind, args.config};
        std::error_code ec;
        if (!fs::is_regular_file(args.config, ec)) {
            die("config file not found: " + args.config);
            return std::nullopt;
        }
    } else {
        auto found = autodetect_config(esp);
        if (found.empty()) {
            die("no bootloader config found on " + esp +
                " (looked for limine.conf, limine.cfg, limine/, grub/grub.cfg,"
                " loader/loader.conf); use --config FILE");
            return std::nullopt;
        }
        source = found[0];
        rep.note("using " + source.first + " config: " + source.second);
        for (size_t i = 1; i < found.size(); ++i)
            rep.note("also found " + found[i].first + " config at " +
                     found[i].second + " (not used; --config to override)");
    }
    if (!args.config.empty())
        rep.note("using " + source.first + " config: " + source.second);

    ParsedConfig parsed;
    try {
        parsed = parse_source(source.first, source.second, esp, ctx, rep);
    } catch (const std::exception& e) {
        die("cannot read " + source.second + ": " + e.what());
        return std::nullopt;
    }

    std::vector<OutNode> roots =
        cap_entries(parsed.roots, args.max_entries, rep);
    for (auto& [e, path] : flatten_entries(roots)) {
        (void)path;
        validate_entry(*e, rep);
    }
    // Check for duplicate entries (same title + kernel)
    {
        auto flat = flatten_entries(roots);
        std::vector<std::shared_ptr<OutEntry>> entry_ptrs;
        entry_ptrs.reserve(flat.size());
        for (auto& [e, path] : flat) entry_ptrs.push_back(e);
        detect_duplicate_entries(entry_ptrs, rep);
    }
    std::string default_str =
        resolve_default(parsed, roots, args.default_entry, rep);
    auto [background, wallpaper_job] = prepare_wallpaper(parsed, ctx, rep);
    std::string cfg_text = emit_config(parsed, roots, default_str, background,
                                       !args.no_extras);

    int n_entries = static_cast<int>(flatten_entries(roots).size());
    int n_submenus = count_submenus(roots);
    int n_rows = n_entries + n_submenus +
                 (args.no_extras ? 0 : static_cast<int>(EXTRAS.size()));
    if (n_rows > MAX_ROWS)
        rep.warn("generated config has " + std::to_string(n_rows) +
                 " menu rows; the firmware supports at most " +
                 std::to_string(MAX_ROWS) +
                 " - reduce entries (see --max-entries)");

    BuildResult res;
    res.cfg_text = cfg_text;
    res.parsed = parsed;
    res.roots = roots;
    res.default_str = default_str;
    res.background = background;
    res.wallpaper = wallpaper_job;
    res.n_entries = n_entries;
    res.n_submenus = n_submenus;
    res.n_rows = n_rows;
    res.esp = esp;
    return res;
}

// ===========================================================================
//  Commands
// ===========================================================================
static std::vector<std::string> entry_summary(const OutEntry& e) {
    std::vector<std::string> lines;
    if (e.type == "linux") {
        lines.push_back("vmlinuz=" + e.vmlinuz);
        if (!e.initrd.empty()) lines.push_back("initrd=" + e.initrd);
    } else if (e.type == "forest") {
        lines.push_back("kernel=" + e.kernel);
        for (const auto& m : e.modules) lines.push_back("module=" + m);
    } else if (e.type == "chainload") {
        if (!e.chain.empty()) lines.push_back("chain=" + e.chain);
    }
    if (!e.cmdline.empty()) {
        std::string c = e.cmdline.size() <= 60 ? e.cmdline
                                               : e.cmdline.substr(0, 57) + "...";
        lines.push_back("cmdline=\"" + c + "\"");
    }
    return lines;
}

int cmd_scan(const Args& args, Reporter& rep) {
    auto result = build_config(args, rep);
    if (!result) return 1;
    const ParsedConfig& cfg = result->parsed;
    std::cout << "ESP:           " << result->esp << "\n";
    std::cout << "Source config: " << cfg.source_path << " (" << cfg.kind
              << ")\n\n";
    std::cout << "Entry tree:\n";

    std::function<void(const std::vector<OutNode>&, const std::string&)> walk =
        [&](const std::vector<OutNode>& nodes, const std::string& prefix) {
            for (size_t i = 0; i < nodes.size(); ++i) {
                const OutNode& n = nodes[i];
                bool last = i == nodes.size() - 1;
                std::string conn = last ? "\xe2\x94\x94\xe2\x94\x80 "
                                        : "\xe2\x94\x9c\xe2\x94\x80 ";
                std::string cont = last ? "   " : "\xe2\x94\x82  ";
                if (n.is_group()) {
                    std::string extra = n.group->icon.empty()
                        ? "" : " (icon=" + n.group->icon + ")";
                    std::cout << prefix << conn << "[submenu] "
                              << n.group->title << extra << "\n";
                    walk(n.group->children, prefix + cont);
                } else {
                    std::string extra = n.entry->icon.empty()
                        ? "" : " (icon=" + n.entry->icon + ")";
                    std::cout << prefix << conn << "[" << n.entry->type << "] "
                              << n.entry->title << extra << "\n";
                    for (const auto& d : entry_summary(*n.entry))
                        std::cout << prefix << cont << "   " << d << "\n";
                }
            }
        };
    walk(result->roots, "  ");
    if (!args.no_extras)
        std::cout << "  + " << EXTRAS.size()
                  << " ForeB utility entries (shell/recovery/tools/setup/"
                     "reboot)\n";
    std::cout << "\nWould emit:\n";
    std::cout << "  entries:   " << result->n_entries << " (+"
              << (args.no_extras ? 0 : (int)EXTRAS.size()) << " utility)\n";
    std::cout << "  submenus:  " << result->n_submenus << "\n";
    std::cout << "  menu rows: " << result->n_rows << "/" << MAX_ROWS << "\n";
    if (cfg.timeout.has_value())
        std::cout << "  timeout:   " << *cfg.timeout << "\n";
    std::cout << "  default:   " << result->default_str << "\n";
    if (cfg.remember_last) std::cout << "  remember_last: 1\n";
    if (result->background)
        std::cout << "  background: " << *result->background << " (from "
                  << (cfg.wallpaper_raw ? *cfg.wallpaper_raw
                      : cfg.wallpaper ? *cfg.wallpaper : "")
                  << ")\n";
    else
        std::cout << "  background: (none)\n";
    if (cfg.kind == "grub")
        std::cout << "\nNOTE: grub.cfg parsing is best-effort; verify the "
                     "result.\n";
    if (!rep.warnings.empty()) {
        std::cout << "\nWarnings (" << rep.warnings.size() << "):\n";
        for (const auto& w : rep.warnings) std::cout << "  - " << w << "\n";
    }
    if (rep.verbose && !rep.notes.empty()) {
        std::cout << "\nNotes (" << rep.notes.size() << "):\n";
        for (const auto& n : rep.notes) std::cout << "  - " << n << "\n";
    }
    return 0;
}

int cmd_generate(const Args& args, Reporter& rep) {
    auto result = build_config(args, rep);
    if (!result) return 1;
    if (!args.output.empty()) {
        try {
            atomic_write(args.output, result->cfg_text);
        } catch (const std::exception& e) {
            return die("cannot write " + args.output + ": " + e.what());
        }
        rep.note("wrote " + args.output);
    } else {
        std::cout << result->cfg_text;
    }
    return 0;
}

std::string safe_rel_path(const std::string& rel_in) {
    std::string rel = replace_all(rel_in, "\\", "/");
    size_t i = 0;
    while (i < rel.size() && rel[i] == '/') ++i;
    rel = rel.substr(i);
    if (starts_with(rel, "EFI/forb/") || rel == "EFI/forb" ||
        starts_with(rel, "forebo/") || rel == "forebo")
        return rel;
    throw std::runtime_error(
        "refusing to write outside EFI/forb and forebo/: " + rel);
}

static std::string shell_join(const std::vector<std::string>& cmd) {
    std::string out;
    for (size_t i = 0; i < cmd.size(); ++i) {
        if (i) out += " ";
        const std::string& a = cmd[i];
        bool need = a.empty();
        for (char c : a)
            if (!(std::isalnum((unsigned char)c) || c == '/' || c == '.' ||
                  c == '_' || c == '-' || c == '\\' || c == ',' || c == ':'))
                need = true;
        if (need) out += "'" + replace_all(a, "'", "'\\''") + "'";
        else out += a;
    }
    return out;
}

static void nvram_install(const std::string& esp, bool make_default, bool dry,
                          bool force, Reporter& rep) {
    std::string prefix = dry ? "[dry-run] " : "";
    if (!which("efibootmgr")) {
        rep.warn("efibootmgr not found; skipping NVRAM registration (create "
                 "the boot entry manually or install efibootmgr)");
        return;
    }
    auto query = [&]() -> std::string {
        auto out = run_cmd({"efibootmgr", "-v"});
        return out ? *out : "";
    };
    auto foreb_entry = [&](const std::string& text) -> std::optional<std::string> {
        static const std::regex boot_re(R"(^Boot([0-9A-Fa-f]{4})\*?\s+(.*)$)");
        static const std::regex file_re(R"(File\(([^)]*)\))");
        for (const auto& line : splitlines(text)) {
            std::smatch m;
            if (!std::regex_match(line, m, boot_re)) continue;
            std::string tail = m[2].str();
            std::smatch pm;
            std::string path =
                std::regex_search(tail, pm, file_re) ? pm[1].str() : tail;
            if (lower(path).find("forb") != std::string::npos) {
                std::string num = m[1].str();
                for (char& c : num) c = std::toupper((unsigned char)c);
                return num;
            }
        }
        return std::nullopt;
    };

    std::string text = dry ? "" : query();
    if (dry)
        std::cout << prefix
                  << "+ efibootmgr -v   # check for an existing ForeB entry\n";
    auto num = foreb_entry(text);
    if (num.has_value() && !force) {
        std::cout << prefix << "UEFI boot entry for ForeB already exists (Boot"
                  << *num << "); skipping creation (use --force to re-register)\n";
    } else {
        if (num.has_value() && force) {
            rep.verbose_out("Re-registering NVRAM entry (Boot" + *num + 
                           ") due to --force flag");
        }
        auto out = run_cmd({"findmnt", "-no", "SOURCE", esp});
        std::string dev;
        if (out && !strip(*out).empty()) {
            auto ls = splitlines(strip(*out));
            if (!ls.empty()) dev = strip(ls[0]);
        }
        auto dp = dev.empty()
            ? std::make_pair(std::optional<std::string>(),
                             std::optional<std::string>())
            : split_disk_part(dev);
        if (!dp.first || !dp.second) {
            rep.warn("cannot determine the ESP disk/partition for " + esp +
                     " (found '" + dev + "'); skipping NVRAM registration");
            return;
        }
        std::vector<std::string> cmd = {
            "efibootmgr", "-c", "-d", *dp.first, "-p", *dp.second, "-L",
            "ForeB", "-l", "\\EFI\\forb\\BOOTX64.EFI"};
        std::cout << prefix << "+ " << shell_join(cmd) << "\n";
        if (!dry) {
            auto r = run_cmd(cmd);
            if (!r) {
                rep.warn("efibootmgr failed");
                return;
            }
            num = foreb_entry(query());
            if (!num) {
                rep.warn("could not find the new ForeB entry after creation");
                return;
            }
            std::cout << "created UEFI boot entry Boot" << *num << " (ForeB)\n";
        }
    }
    if (make_default) {
        if (dry) {
            std::cout << prefix << "+ efibootmgr -o <ForeB>,<rest>   # put "
                                  "ForeB first in BootOrder\n";
            return;
        }
        std::string t = query();
        std::vector<std::string> order;
        static const std::regex order_re(R"(^BootOrder:\s*(.*)$)");
        for (const auto& line : splitlines(t)) {
            std::smatch m;
            if (std::regex_match(line, m, order_re)) {
                std::istringstream iss(m[1].str());
                std::string tok;
                while (std::getline(iss, tok, ',')) {
                    std::string s = strip(tok);
                    for (char& c : s) c = std::toupper((unsigned char)c);
                    if (!s.empty()) order.push_back(s);
                }
            }
        }
        if (num.has_value() && !order.empty()) {
            std::vector<std::string> nw = {*num};
            for (const auto& x : order) if (x != *num) nw.push_back(x);
            std::vector<std::string> cmd = {"efibootmgr", "-o", join(",", nw)};
            std::cout << "+ " << shell_join(cmd) << "\n";
            if (!run_cmd(cmd)) rep.warn("efibootmgr -o failed");
        } else {
            rep.warn("cannot reorder BootOrder (entry or order unknown)");
        }
    }
}

int cmd_install(const Args& args, Reporter& rep) {
    bool dry = args.dry_run;
    if (::geteuid() != 0 && !dry)
        return die("install requires root - re-run with sudo (or use "
                   "--dry-run to preview the actions)");
    auto result = build_config(args, rep);
    if (!result) return 1;
    const std::string& esp = result->esp;
    const std::string& repo = args.repo;

    // Verbose output: show detected bootloader info
    rep.verbose_out("Detected " + result->parsed.kind + " at " + 
                    result->parsed.source_path + " (" + 
                    std::to_string(result->n_entries) + " entries, " + 
                    std::to_string(result->n_submenus) + " submenus)");
    rep.verbose_out("Translated " + std::to_string(result->n_entries) + 
                    " entries to ForeB format");

    auto do_action = [&](const std::string& desc, const std::function<void()>& fn) {
        std::cout << (dry ? "[dry-run] " : "") << desc << "\n";
        if (!dry) fn();
    };
    auto copy_file = [&](const std::string& src, const std::string& rel_in) {
        std::string rel = safe_rel_path(rel_in);
        std::string dest = (fs::path(esp) / rel).string();
        do_action("copy " + src + " -> " + dest, [&, dest]() {
            fs::create_directories(fs::path(dest).parent_path());
            fs::copy_file(src, dest, fs::copy_options::overwrite_existing);
        });
    };
    auto write_file = [&](const std::string& rel_in, const std::string& data) {
        std::string rel = safe_rel_path(rel_in);
        std::string dest = (fs::path(esp) / rel).string();
        rep.verbose_out("Writing " + dest + " (" + std::to_string(data.size()) + " bytes)");
        do_action("write " + dest + " (" + std::to_string(data.size()) +
                      " bytes)",
                  [&, dest]() {
                      fs::create_directories(fs::path(dest).parent_path());
                      atomic_write(dest, data);
                  });
    };

    // Map an ESP-relative payload path to its --repo override source, if any.
    auto repo_override = [&](const std::string& rel) -> std::string {
        if (repo.empty()) return "";
        std::string src;
        if (rel == "EFI/forb/BOOTX64.EFI")
            src = (fs::path(repo) / "BOOTX64.EFI").string();
        else if (rel == "forebo/bg.bmp")
            src = (fs::path(repo) / "assets" / "bg.bmp").string();
        else if (starts_with(rel, "forebo/icons/"))
            src = (fs::path(repo) / "assets" / "icons" /
                   rel.substr(std::string("forebo/icons/").size())).string();
        std::error_code ec;
        if (!src.empty() && fs::is_regular_file(src, ec)) return src;
        return "";
    };

    bool have_loader = false;
    // Extract the embedded payload (loader + assets). forebo.cfg is replaced
    // by the translated config below, so it is skipped here.
    for (const auto& pf : payload_files()) {
        if (pf.name == "forebo/forebo.cfg") continue;
        std::string over = repo_override(pf.name);
        if (!over.empty()) copy_file(over, pf.name);
        else write_file(pf.name, pf.data);
        if (pf.name == "EFI/forb/BOOTX64.EFI") have_loader = true;
    }
    if (!have_loader) {
        // Fall back to --repo BOOTX64.EFI if the embedded blob lacked it.
        std::string over = repo_override("EFI/forb/BOOTX64.EFI");
        if (!over.empty()) { copy_file(over, "EFI/forb/BOOTX64.EFI"); }
        else if (dry)
            std::cout << "[dry-run] WARNING: no embedded BOOTX64.EFI and no "
                         "--repo override; build ForeB first (make)\n";
        else
            return die("no BOOTX64.EFI available (embedded payload empty and "
                       "no --repo override) - build ForeB first (make)");
    }

    // wallpaper (converted when needed)
    if (result->wallpaper) {
        write_file("forebo/" + result->wallpaper->first,
                   result->wallpaper->second);
    }

    // forebo.cfg (translated)
    write_file("forebo/forebo.cfg", result->cfg_text);

    // NVRAM
    if (args.no_nvram)
        std::cout << (dry ? "[dry-run] " : "")
                  << "skipping efibootmgr (--no-nvram)\n";
    else
        nvram_install(esp, args.make_default, dry, args.force, rep);

    // summary
    if (Color::enabled)
        std::cout << "\n" << Color::green << Color::bold 
                  << "ForeB install complete!" << Color::reset << "\n";
    else
        std::cout << "\nForeB install complete!\n";
    std::cout << "  ESP:      " << esp << "\n";
    std::cout << "  Loader:   "
              << (fs::path(esp) / "EFI/forb/BOOTX64.EFI").string() << "\n";
    std::cout << "  Config:   "
              << (fs::path(esp) / "forebo/forebo.cfg").string() << " ("
              << result->n_entries << " entries, " << result->n_submenus
              << " submenus, " << result->n_rows << "/" << MAX_ROWS
              << " rows)\n";
    std::cout << "  Assets:   " << (fs::path(esp) / "forebo/bg.bmp").string()
              << ", " << (fs::path(esp) / "forebo/icons/*.tga").string()
              << "\n";
    if (result->background)
        std::cout << "  Wallpaper: "
                  << (fs::path(esp) / result->background->substr(1)).string()
                  << "\n";
    std::cout << "\nHow to boot it: pick 'ForeB' in your firmware boot menu, "
                 "or select it from the UEFI boot list.\n";
    if (args.make_default)
        std::cout << "ForeB was made the FIRST entry in the UEFI BootOrder "
                     "(--make-default).\n";
    else
        std::cout << "Your existing bootloader was left untouched and remains "
                     "the default; the ForeB entry was appended to the boot "
                     "list.\n";
    return 0;
}

// ===========================================================================
//  Config validation (lint)
// ===========================================================================
int LintResult::errors() const {
    int n = 0;
    for (auto& m : messages) if (m.level == LintMessage::Error) ++n;
    return n;
}
int LintResult::warnings() const {
    int n = 0;
    for (auto& m : messages) if (m.level == LintMessage::Warning) ++n;
    return n;
}
int LintResult::infos() const {
    int n = 0;
    for (auto& m : messages) if (m.level == LintMessage::Info) ++n;
    return n;
}
void LintResult::error(const std::string& msg) {
    messages.push_back({LintMessage::Error, msg});
}
void LintResult::warn(const std::string& msg) {
    messages.push_back({LintMessage::Warning, msg});
}
void LintResult::info(const std::string& msg) {
    messages.push_back({LintMessage::Info, msg});
}

// --- forebo.cfg text-level validation (syntax + limits) ---
static void lint_syntax(const std::string& text, const std::string& esp,
                         LintResult& lr) {
    auto lines = splitlines(text);

    // Balanced braces
    int depth = 0;
    int lineno = 0;
    for (const auto& raw : lines) {
        ++lineno;
        std::string line = strip(raw);
        // skip comments
        size_t comment = line.find('#');
        if (comment != std::string::npos) line = strip(line.substr(0, comment));
        for (char c : line) {
            if (c == '{') ++depth;
            else if (c == '}') --depth;
            if (depth < 0) {
                lr.error("line " + std::to_string(lineno) +
                         ": unexpected '}' without matching '{'");
                depth = 0;
            }
        }
    }
    if (depth > 0)
        lr.error("unbalanced braces: " + std::to_string(depth) +
                 " unclosed block(s) at EOF");
    else if (depth == 0 && !lines.empty())
        lr.info("braces balanced");

    // Valid key=value pairs (outside blocks, top-level)
    depth = 0;
    lineno = 0;
    for (const auto& raw : lines) {
        ++lineno;
        std::string line = strip(raw);
        if (line.empty()) continue;
        // skip comments
        size_t comment = line.find('#');
        if (comment != std::string::npos) line = strip(line.substr(0, comment));
        if (line.empty()) continue;

        for (char c : line) {
            if (c == '{') ++depth;
            else if (c == '}') --depth;
        }

        // Inside a block: check menuentry/submenu headers
        if (depth > 0 || (depth == 0 && line.find('{') != std::string::npos)) {
            // block header - ok
            continue;
        }
        // Top-level line outside blocks
        if (depth == 0) {
            // valid top-level keys: timeout, default, remember_last, background,
            // menuentry, submenu
            static const std::set<std::string> top_keys = {
                "timeout", "default", "remember_last", "background",
                "menuentry", "submenu"};
            size_t eq = line.find('=');
            if (eq != std::string::npos) {
                std::string key = strip(line.substr(0, eq));
                if (top_keys.find(key) == top_keys.end()) {
                    lr.warn("line " + std::to_string(lineno) +
                            ": unknown top-level key '" + key + "'");
                }
            }
        }
    }

    // Check for entry count (NVRAM limit)
    int entry_count = 0;
    int submenu_count = 0;
    depth = 0;
    for (const auto& raw : lines) {
        std::string line = strip(raw);
        size_t comment = line.find('#');
        if (comment != std::string::npos) line = strip(line.substr(0, comment));
        if (line.empty()) continue;
        for (char c : line) {
            if (c == '{') ++depth;
            else if (c == '}') --depth;
        }
        if (starts_with(line, "menuentry ") && line.find('{') != std::string::npos)
            ++entry_count;
        if (starts_with(line, "submenu ") && line.find('{') != std::string::npos)
            ++submenu_count;
    }
    if (entry_count > MAX_ROWS)
        lr.error("entry count " + std::to_string(entry_count) +
                 " exceeds firmware limit (" + std::to_string(MAX_ROWS) +
                 "); entries will be dropped");
    else if (entry_count > MAX_ROWS - (int)EXTRAS.size())
        lr.warn("entry count " + std::to_string(entry_count) +
                " is close to firmware limit (" + std::to_string(MAX_ROWS) +
                " with " + std::to_string(EXTRAS.size()) +
                " utility entries); consider reducing");

    // Path/title/cmdline length checks per line
    lineno = 0;
    for (const auto& raw : lines) {
        ++lineno;
        std::string line = strip(raw);
        if (line.empty() || line[0] == '#') continue;

        // title check
        {
            static const std::regex title_re(
                R"(^(menuentry|submenu)\s+\"([^\"]*)\")");
            std::smatch m;
            if (std::regex_match(line, m, title_re)) {
                std::string title = m[2].str();
                if ((int)title.size() > MAX_TITLE)
                    lr.warn("line " + std::to_string(lineno) +
                            ": title '" + title.substr(0, 30) + "...' is " +
                            std::to_string(title.size()) +
                            " chars (max " + std::to_string(MAX_TITLE) + ")");
            }
        }

        // path keys
        for (const char* pk : {"vmlinuz=", "kernel=", "initrd=", "chain=",
                                "module=", "background="}) {
            if (starts_with(line, pk)) {
                std::string val = line.substr(std::strlen(pk));
                if ((int)val.size() > MAX_PATH)
                    lr.warn("line " + std::to_string(lineno) +
                            ": path '" + std::string(pk) + "' is " +
                            std::to_string(val.size()) +
                            " chars (max " + std::to_string(MAX_PATH) +
                            ")");
            }
        }

        // cmdline length
        if (starts_with(line, "cmdline=\"")) {
            std::string val = line.substr(std::strlen("cmdline=\""));
            if (!val.empty() && val.back() == '"') val.pop_back();
            if ((int)val.size() > MAX_CMDLINE)
                lr.warn("line " + std::to_string(lineno) +
                        ": cmdline is " + std::to_string(val.size()) +
                        " chars (max " + std::to_string(MAX_CMDLINE) + ")");
        }
    }

    // ESP file existence checks
    if (!esp.empty()) {
        lineno = 0;
        for (const auto& raw : lines) {
            ++lineno;
            std::string line = strip(raw);
            if (line.empty() || line[0] == '#') continue;

            for (const char* pk : {"vmlinuz=", "kernel=", "initrd=", "chain=",
                                    "module=", "background="}) {
                if (starts_with(line, pk)) {
                    std::string val = strip(line.substr(std::strlen(pk)));
                    if (val.empty() || val[0] != '/') continue;
                    std::string full = (fs::path(esp) / val.substr(1)).string();
                    std::error_code ec;
                    if (!fs::is_regular_file(full, ec)) {
                        lr.warn("line " + std::to_string(lineno) + ": " +
                                std::string(pk) + val + " not found on ESP");
                    }
                }
            }
        }
    }
}

// --- Validate from parsed ParsedConfig (entry-level checks) ---
static void lint_parsed(const ParsedConfig& parsed, const std::string& esp,
                         LintResult& lr) {
    auto flat = flatten_entries(parsed.roots);

    // Entry count
    if ((int)flat.size() > MAX_ROWS)
        lr.error("parsed entry count " + std::to_string(flat.size()) +
                 " exceeds firmware limit (" + std::to_string(MAX_ROWS) + ")");

    // Duplicate titles
    std::set<std::string> titles;
    for (auto& [e, path] : flat) {
        if (!titles.insert(e->title).second)
            lr.warn("duplicate entry title: '" + e->title + "'");
    }

    // Per-entry checks
    for (auto& [e, path] : flat) {
        if ((int)e->title.size() > MAX_TITLE)
            lr.warn("entry '" + e->title.substr(0, 30) +
                    "...' title exceeds max length");
        if ((int)e->cmdline.size() > MAX_CMDLINE)
            lr.warn("entry '" + e->title + "' cmdline exceeds max length");
        for (const auto& m : e->modules)
            if ((int)m.size() > MAX_PATH)
                lr.warn("entry '" + e->title +
                        "' module path exceeds max length");

        // Missing icon
        if (e->icon.empty())
            lr.warn("entry '" + e->title + "' has no icon assigned");

        // Invalid type
        if (e->type != "linux" && e->type != "forest" &&
            e->type != "chainload" && e->type != "shell" &&
            e->type != "recovery" && e->type != "tools" &&
            e->type != "setup" && e->type != "reboot")
            lr.warn("entry '" + e->title + "' has unknown type '" + e->type +
                    "'");

        // ESP file existence
        if (!esp.empty()) {
            auto check_path = [&](const std::string& label,
                                   const std::string& p) {
                if (p.empty() || p[0] != '/') return;
                std::string full = (fs::path(esp) / p.substr(1)).string();
                std::error_code ec;
                if (!fs::is_regular_file(full, ec))
                    lr.warn("entry '" + e->title + "': " + label + " " + p +
                            " not found on ESP");
            };
            check_path("vmlinuz", e->vmlinuz);
            check_path("kernel", e->kernel);
            check_path("initrd", e->initrd);
            check_path("chain", e->chain);
            for (const auto& m : e->modules) check_path("module", m);
        }
    }
}

LintResult validate_config(const std::string& text, const std::string& esp) {
    LintResult lr;
    if (text.empty()) {
        lr.error("config text is empty");
        return lr;
    }
    lint_syntax(text, esp, lr);
    return lr;
}

LintResult validate_config(const ParsedConfig& parsed, const std::string& esp,
                            const std::string& text) {
    LintResult lr;
    if (!text.empty()) lint_syntax(text, esp, lr);
    lint_parsed(parsed, esp, lr);
    return lr;
}

static void print_lint_result(const LintResult& lr) {
    for (const auto& m : lr.messages) {
        const char* tag = "?";
        if (m.level == LintMessage::Error) tag = "error";
        else if (m.level == LintMessage::Warning) tag = "warning";
        else tag = "info";
        std::cerr << tag << ": " << m.text << "\n";
    }
    int total = lr.errors() + lr.warnings() + lr.infos();
    if (total == 0) {
        std::cout << "clean\n";
    } else {
        std::cout << lr.infos() << " info, " << lr.warnings() << " warnings, "
                  << lr.errors() << " errors\n";
    }
}

int cmd_lint(const Args& args, Reporter& rep) {
    // The config path is the first positional argument (not a flag).
    // In main.cpp we stored it in args.config for lint.
    std::string cfg_path = args.config;
    if (cfg_path.empty()) {
        std::cerr << TOOL << " lint: error: no config file specified\n"
                  << "usage: " << TOOL << " lint [--esp PATH] FILE\n";
        return 2;
    }
    std::error_code ec;
    if (!fs::is_regular_file(cfg_path, ec)) {
        std::cerr << TOOL << " lint: error: " << cfg_path
                  << " not found\n";
        return 1;
    }

    std::string text;
    try {
        text = read_text(cfg_path);
    } catch (const std::exception& e) {
        std::cerr << TOOL << " lint: error: cannot read " << cfg_path << ": "
                  << e.what() << "\n";
        return 1;
    }

    // First pass: syntax-only validation on raw text
    LintResult lr = validate_config(text, args.esp);

    // Second pass: parse and validate entry-level details
    try {
        std::string kind = infer_kind(cfg_path);
        std::string esp = args.esp;
        if (esp.empty()) {
            auto d = detect_esp();
            if (d) esp = *d;
        }
        Reporter parse_rep(false);
        EspContext ctx(esp.empty() ? "/dev/null" : esp, parse_rep);
        ParsedConfig parsed;
        if (kind == "limine")
            parsed = parse_limine(cfg_path, ctx, parse_rep);
        else if (kind == "grub")
            parsed = parse_grub(cfg_path, ctx, parse_rep);
        else if (kind == "systemd-boot")
            parsed = parse_systemd_boot(esp.empty() ? "" : esp, cfg_path,
                                         parse_rep);
        LintResult lr2 = validate_config(parsed, esp, text);
        for (auto& m : lr2.messages) lr.messages.push_back(std::move(m));
    } catch (const std::exception& e) {
        lr.warn("could not parse config for deep validation: " +
                std::string(e.what()));
    }

    print_lint_result(lr);
    if (lr.has_errors()) return 1;
    if (lr.has_warnings()) return 1;
    return 0;
}

// ===========================================================================
//  uninstall
// ===========================================================================
static std::optional<std::string> find_foreb_nvram(const std::string& esp,
                                                   bool dry) {
    if (!which("efibootmgr")) return std::nullopt;
    auto out = run_cmd({"efibootmgr", "-v"});
    if (!out) return std::nullopt;
    static const std::regex boot_re(R"(^Boot([0-9A-Fa-f]{4})\*?\s+(.*)$)");
    static const std::regex file_re(R"(File\(([^)]*)\))");
    for (const auto& line : splitlines(*out)) {
        std::smatch m;
        if (!std::regex_match(line, m, boot_re)) continue;
        std::string tail = m[2].str();
        std::smatch pm;
        std::string path =
            std::regex_search(tail, pm, file_re) ? pm[1].str() : tail;
        if (lower(path).find("forb") != std::string::npos) {
            std::string num = m[1].str();
            for (char& c : num) c = std::toupper((unsigned char)c);
            return num;
        }
    }
    return std::nullopt;
}

int cmd_uninstall(const Args& args, Reporter& rep) {
    bool dry = args.dry_run;
    if (::geteuid() != 0 && !dry)
        return die("uninstall requires root - re-run with sudo (or use "
                   "--dry-run to preview)");

    std::string esp = args.esp;
    if (esp.empty()) {
        auto d = detect_esp();
        if (!d)
            return die("no ESP found: use --esp PATH");
        esp = *d;
        rep.note("auto-detected ESP at " + esp);
    }

    std::string prefix = dry ? "[dry-run] " : "";
    fs::path esp_root(esp);
    std::string forb_dir = (esp_root / "EFI" / "forb").string();
    std::string forebo_dir = (esp_root / "forebo").string();

    bool has_forb = fs::is_directory(forb_dir);
    bool has_forebo = fs::is_directory(forebo_dir);

    auto nvram_entry = find_foreb_nvram(esp, dry);
    bool has_nvram = nvram_entry.has_value();

    if (!has_forb && !has_forebo && !has_nvram) {
        std::cout << "Nothing to remove: no ForeB installation found at " << esp
                  << "\n";
        return 0;
    }

    // Show what would be removed
    std::cout << "ForeB components found at " << esp << ":\n";
    if (has_forb)
        std::cout << "  ESP files:  " << forb_dir << "\n";
    if (has_forebo)
        std::cout << "  ESP files:  " << forebo_dir << "\n";
    if (has_nvram)
        std::cout << "  NVRAM:      Boot" << *nvram_entry << " (ForeB)\n";
    std::cout << "\n";

    // Confirmation prompt
    if (!args.yes && !dry) {
        std::cout << "This will permanently remove ForeB from this system.\n";
        std::cout << "Proceed? [y/N] ";
        std::cout.flush();
        char c = 0;
        if (std::cin.get(c) && (c != 'y' && c != 'Y')) {
            std::cout << "Aborted.\n";
            return 0;
        }
    }

    // Remove ESP directories
    auto remove_dir = [&](const std::string& path, const std::string& label) {
        std::error_code ec;
        if (!fs::is_directory(path, ec)) return;
        std::cout << prefix << "remove " << path << "\n";
        if (!dry) {
            fs::remove_all(path, ec);
            if (ec)
                rep.warn("failed to remove " + path + ": " + ec.message());
        }
    };
    remove_dir(forb_dir, "EFI/forb");
    remove_dir(forebo_dir, "forebo");

    // Remove NVRAM entry
    if (has_nvram && !args.keep_nvram) {
        std::cout << prefix << "+ efibootmgr -b " << *nvram_entry << " -B\n";
        if (!dry) {
            auto r = run_cmd({"efibootmgr", "-b", *nvram_entry, "-B"});
            if (!r) {
                rep.warn("efibootmgr -b " + *nvram_entry +
                         " -B failed; remove the entry manually");
            } else {
                std::cout << "Removed UEFI boot entry Boot" << *nvram_entry
                          << "\n";
            }
        }
    } else if (has_nvram && args.keep_nvram) {
        std::cout << prefix << "keeping NVRAM entry Boot" << *nvram_entry
                  << " (--keep-nvram)\n";
    }

    std::cout << "\nForeB uninstalled" << (dry ? " (dry-run)" : "") << ".\n";
    return 0;
}

// ===========================================================================
//  Bootloader detection
// ===========================================================================
struct DetectResult {
    std::string name;
    double confidence;
    std::string config_path;
};

static void check_file(const std::string& path, const std::string& name,
                       double base_conf, std::vector<DetectResult>& out) {
    std::error_code ec;
    if (fs::is_regular_file(path, ec)) {
        out.push_back({name, base_conf, path});
    }
}

static void check_content(const std::string& path, const std::string& name,
                          double base_conf, const std::regex& re,
                          std::vector<DetectResult>& out) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return;
    try {
        std::string head = read_text(path);
        if (head.size() > 8192) head = head.substr(0, 8192);
        if (std::regex_search(head, re)) {
            out.push_back({name, base_conf, path});
        }
    } catch (...) {}
}

std::vector<BootloaderInfo> detect_bootloader(const std::string& dir) {
    std::vector<BootloaderInfo> results;
    fs::path root(dir);

    // Collect raw detections
    std::vector<DetectResult> raw;

    // ---- ForeB (highest priority) ----
    {
        std::error_code ec;
        if (fs::is_regular_file(root / "forebo" / "forebo.cfg", ec)) {
            raw.push_back({"foreb", 1.0,
                           (root / "forebo" / "forebo.cfg").string()});
        }
    }

    // ---- GRUB ----
    check_file((root / "grub" / "grub.cfg").string(), "grub", 0.9, raw);
    // Content-based: look for menuentry
    {
        static const std::regex grub_re(R"(menuentry\s+)");
        check_content((root / "grub" / "grub.cfg").string(), "grub", 0.85,
                      grub_re, raw);
    }

    // ---- Limine ----
    for (const auto& name : {"limine.cfg", "limine.conf"}) {
        check_file((root / name).string(), "limine", 0.9, raw);
    }
    for (const auto& sub : {"limine", "boot/limine"}) {
        for (const auto& name : {"limine.cfg", "limine.conf"}) {
            check_file((root / sub / name).string(), "limine", 0.88, raw);
        }
    }
    {
        static const std::regex limine_re(
            R"(^/+\+?|^\s*protocol\s*:)", std::regex::multiline);
        for (const auto& cand : {root / "limine.cfg", root / "limine.conf",
                                 root / "limine" / "limine.cfg",
                                 root / "limine" / "limine.conf"}) {
            check_content(cand.string(), "limine", 0.85, limine_re, raw);
        }
    }

    // ---- systemd-boot ----
    check_file((root / "loader" / "loader.conf").string(), "systemd-boot",
               0.9, raw);
    {
        std::error_code ec;
        std::string entries_dir = (root / "loader" / "entries").string();
        if (fs::is_directory(entries_dir, ec)) {
            int conf_count = 0;
            for (const auto& e : fs::directory_iterator(entries_dir, ec)) {
                if (ends_with(e.path().filename().string(), ".conf"))
                    ++conf_count;
            }
            if (conf_count > 0) {
                // Boost confidence if loader.conf + entries both exist
                for (auto& r : raw) {
                    if (r.name == "systemd-boot") {
                        r.confidence = std::min(1.0, r.confidence + 0.05);
                    }
                }
                if (raw.empty() ||
                    raw.back().name != "systemd-boot") {
                    raw.push_back({"systemd-boot", 0.7,
                                   (root / "loader" / "loader.conf").string()});
                }
            }
        }
    }

    // ---- rEFInd ----
    for (const auto& sub : {"EFI/refind", "efi/refind"}) {
        check_file((root / sub / "refind.conf").string(), "refind", 0.88, raw);
        // Also check for refind.conf in root
        check_file((root / "boot" / "refind.conf").string(), "refind", 0.82,
                   raw);
    }
    {
        static const std::regex refind_re(
            R"(^\s*(?:menuentry|include)\b)", std::regex::multiline);
        for (const auto& sub : {"EFI/refind", "efi/refind"}) {
            check_content((root / sub / "refind.conf").string(), "refind",
                          0.80, refind_re, raw);
        }
    }

    // ---- Clover ----
    for (const auto& sub : {"EFI/CLOVER", "EFI/clover", "clover"}) {
        check_file((root / sub / "config.plist").string(), "clover", 0.88,
                   raw);
    }

    // ---- syslinux ----
    for (const auto& sub : {"syslinux", "boot/syslinux", "boot/isolinux",
                            "isolinux"}) {
        check_file((root / sub / "syslinux.cfg").string(), "syslinux", 0.88,
                   raw);
        check_file((root / sub / "isolinux.cfg").string(), "syslinux", 0.85,
                   raw);
    }
    {
        static const std::regex syslinux_re(
            R"(^\s*(?:LABEL|KERNEL|APPEND)\b)", std::regex::multiline);
        for (const auto& sub : {"syslinux", "boot/syslinux", "isolinux"}) {
            check_content((root / sub / "syslinux.cfg").string(), "syslinux",
                          0.80, syslinux_re, raw);
        }
    }

    // ---- ZBM (ZFS Boot Menu) ----
    for (const auto& name : {"zbm.ini", "zbm.conf"}) {
        check_file((root / name).string(), "zbm", 0.85, raw);
    }
    check_file((root / "EFI" / "ZBM" / "zbm.ini").string(), "zbm", 0.88, raw);

    // ---- Windows Boot Manager (informational only) ----
    {
        std::error_code ec;
        if (fs::is_regular_file(root / "EFI" / "Microsoft" / "Boot" /
                                "bootmgfw.efi", ec)) {
            raw.push_back({"windows", 0.6,
                           "(BCD store - no direct config)"});
        }
    }

    // Deduplicate: keep best confidence per name
    std::map<std::string, DetectResult> best;
    for (const auto& r : raw) {
        auto it = best.find(r.name);
        if (it == best.end() || r.confidence > it->second.confidence) {
            best[r.name] = r;
        }
    }

    // Sort by priority: ForeB > GRUB > Limine > systemd-boot > rEFInd >
    //   Clover > syslinux > ZBM > windows
    auto priority = [](const std::string& name) -> int {
        if (name == "foreb") return 0;
        if (name == "grub") return 1;
        if (name == "limine") return 2;
        if (name == "systemd-boot") return 3;
        if (name == "refind") return 4;
        if (name == "clover") return 5;
        if (name == "syslinux") return 6;
        if (name == "zbm") return 7;
        return 99;
    };

    std::vector<DetectResult> sorted;
    for (auto& [_, r] : best) sorted.push_back(r);
    std::sort(sorted.begin(), sorted.end(),
              [&](const DetectResult& a, const DetectResult& b) {
                  if (priority(a.name) != priority(b.name))
                      return priority(a.name) < priority(b.name);
                  return a.confidence > b.confidence;
              });

    // Convert to BootloaderInfo with features and hints
    for (const auto& r : sorted) {
        BootloaderInfo info;
        info.name = r.name;
        info.confidence = r.confidence;
        info.config_path = r.config_path;

        if (r.name == "foreb") {
            info.features = {"all"};
            info.migration_path = "already ForeB; no migration needed";
        } else if (r.name == "grub") {
            info.features = {"linux", "chainload", "multiboot", "submenus",
                             "timeout", "default"};
            info.warnings = {
                "grub.cfg is generated (not hand-edited); re-running "
                "grub-mkconfig may overwrite changes",
                "complex grub scripts may not translate fully",
                "GRUB modules (insmod) are not supported by ForeB"};
            info.migration_path =
                "use `forb-install migrate` to convert grub.cfg -> forebo.cfg";
        } else if (r.name == "limine") {
            info.features = {"linux", "chainload", "multiboot", "submenus",
                             "timeout", "wallpaper", "default"};
            info.warnings = {
                "Limine protocol=chainload is BIOS-only and unsupported on "
                "UEFI ForeB",
                "Multiboot2 kernels may not boot (ForeB is Multiboot1-only)"};
            info.migration_path =
                "use `forb-install migrate` to convert limine.conf -> forebo.cfg";
        } else if (r.name == "systemd-boot") {
            info.features = {"linux", "chainload", "timeout", "default"};
            info.warnings = {
                "kernel command-line options are preserved as-is; verify they "
                "are compatible",
                "systemd-boot snapshots/entries may reference non-ESP paths"};
            info.migration_path = "use `forb-install migrate` to convert "
                                  "loader/loader.conf -> forebo.cfg";
        } else if (r.name == "refind") {
            info.features = {"linux", "chainload", "submenus"};
            info.warnings = {
                "rEFInd manual stanza detection is limited",
                "rEFInd icons/themes are not migrated",
                "parser is not yet implemented"};
            info.migration_path =
                "parser not yet implemented; manually create forebo.cfg or "
                "use `forb-install install`";
        } else if (r.name == "clover") {
            info.features = {"linux", "chainload"};
            info.warnings = {
                "Clover uses XML plist format; parser is not yet implemented",
                "Clover themes/kexts are not migrated"};
            info.migration_path =
                "parser not yet implemented; manually create forebo.cfg";
        } else if (r.name == "syslinux") {
            info.features = {"linux", "chainload", "timeout", "default"};
            info.warnings = {
                "COM32/COMBOOT modules are BIOS-only and unsupported on UEFI",
                "LOCALBOOT entries are unsupported",
                "SYSAPPEND flag is ignored by ForeB"};
            info.migration_path = "use `forb-install migrate` to convert "
                                  "syslinux.cfg -> forebo.cfg";
        } else if (r.name == "zbm") {
            info.features = {"linux", "chainload"};
            info.warnings = {
                "ZBM uses a custom INI format; parser is not yet implemented"};
            info.migration_path =
                "parser not yet implemented; manually create forebo.cfg";
        } else if (r.name == "windows") {
            info.features = {};
            info.warnings = {"Windows Boot Manager cannot be migrated; "
                             "ForeB can chainload it"};
            info.migration_path =
                "create a chainload entry pointing to "
                "EFI/Microsoft/Boot/bootmgfw.efi";
        }
        results.push_back(std::move(info));
    }
    return results;
}

// ===========================================================================
//  list
// ===========================================================================
struct EspBootloaderInfo {
    std::string name;
    std::string config_path;
    std::string efi_path;
};

static void scan_esp_bootloaders(const std::string& esp,
                                 std::vector<EspBootloaderInfo>& found) {
    fs::path esp_root(esp);
    auto check = [&](const std::string& name, const std::string& cfg_rel,
                     const std::string& efi_rel) {
        std::error_code ec;
        if (fs::is_regular_file(esp_root / cfg_rel, ec)) {
            found.push_back({name, cfg_rel, efi_rel});
        }
    };

    // ForeB
    {
        std::error_code ec;
        bool has_forb = fs::is_directory(esp_root / "EFI" / "forb", ec);
        bool has_cfg = fs::is_regular_file(esp_root / "forebo" / "forebo.cfg", ec);
        if (has_forb && has_cfg)
            found.push_back({"ForeB", "forebo/forebo.cfg",
                             "EFI/forb/BOOTX64.EFI"});
    }

    // GRUB
    check("GRUB", "grub/grub.cfg", "EFI/grub/grubx64.efi");

    // Limine
    for (const auto& name : {"limine.cfg", "limine.conf"}) {
        std::error_code ec;
        if (fs::is_regular_file(esp_root / name, ec)) {
            found.push_back({"Limine", name, "EFI/limine/limine-x86_64.efi"});
            break;
        }
    }
    {
        std::error_code ec;
        if (fs::is_regular_file(esp_root / "limine" / "limine.cfg", ec) ||
            fs::is_regular_file(esp_root / "limine" / "limine.conf", ec)) {
            found.push_back({"Limine", "limine/limine.cfg",
                             "EFI/limine/limine-x86_64.efi"});
        }
    }

    // systemd-boot
    check("systemd-boot", "loader/loader.conf",
          "EFI/systemd/systemd-bootx64.efi");

    // rEFInd
    {
        std::error_code ec;
        if (fs::is_regular_file(esp_root / "EFI" / "refind" / "refind.conf", ec))
            found.push_back({"rEFInd", "EFI/refind/refind.conf",
                             "EFI/refind/refind_x64.efi"});
    }

    // Clover
    {
        std::error_code ec;
        if (fs::is_regular_file(esp_root / "EFI" / "CLOVER" / "config.plist", ec))
            found.push_back({"Clover", "EFI/CLOVER/config.plist",
                             "EFI/CLOVER/CLOVERX64.efi"});
    }

    // syslinux
    check("syslinux", "syslinux/syslinux.cfg",
          "EFI/syslinux/syslinuxx64.efi");

    // Windows Boot Manager
    {
        std::error_code ec;
        if (fs::is_regular_file(esp_root / "EFI" / "Microsoft" / "Boot" /
                                "bootmgfw.efi", ec))
            found.push_back({"Windows Boot Manager", "(BCD store)",
                             "EFI/Microsoft/Boot/bootmgfw.efi"});
    }
}

int cmd_list(const Args& args, Reporter& rep) {
    std::string esp = args.esp;
    if (esp.empty()) {
        auto d = detect_esp();
        if (!d)
            return die("no ESP found: use --esp PATH");
        esp = *d;
    }

    // ESP bootloaders
    std::vector<EspBootloaderInfo> found;
    scan_esp_bootloaders(esp, found);

    std::cout << "ESP: " << esp << "\n\n";
    if (found.empty()) {
        std::cout << "No bootloaders found on ESP.\n";
    } else {
        std::cout << "Bootloaders on ESP:\n";
        for (const auto& b : found) {
            std::cout << "  " << b.name << "\n"
                      << "    config: " << b.config_path << "\n"
                      << "    EFI:    " << b.efi_path << "\n";
        }
    }

    // NVRAM entries
    std::cout << "\nUEFI NVRAM boot entries:\n";
    if (which("efibootmgr")) {
        auto out = run_cmd({"efibootmgr", "-v"});
        if (out) {
            for (const auto& line : splitlines(*out))
                std::cout << "  " << line << "\n";
        } else {
            std::cout << "  (could not read efibootmgr output)\n";
        }
    } else {
        std::cout << "  efibootmgr not available\n";
    }

    return 0;
}

// ===========================================================================
//  backup
// ===========================================================================
int cmd_backup(const Args& args, Reporter& rep) {
    std::string esp = args.esp;
    if (esp.empty()) {
        auto d = detect_esp();
        if (!d)
            return die("no ESP found: use --esp PATH");
        esp = *d;
        rep.note("auto-detected ESP at " + esp);
    }

    // Determine output path
    std::string outpath = args.output;
    if (outpath.empty()) {
        // Generate timestamped filename
        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        struct tm tm_buf;
        localtime_r(&tt, &tm_buf);
        char ts[32];
        std::strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &tm_buf);
        outpath = "forb-backup-" + std::string(ts) + ".tar.gz";
    }

    std::cout << "Backing up ESP configuration from " << esp << "\n"
              << "Output: " << outpath << "\n\n";

    // Create temp directory for staging
    fs::path tmpdir = fs::path("/tmp") / "forb-backup-XXXXXX";
    std::string tmpdir_str = tmpdir.string();
    {
        std::vector<char> buf(tmpdir_str.begin(), tmpdir_str.end());
        buf.push_back('\0');
        char* r = ::mkdtemp(buf.data());
        if (!r) return die("cannot create temp directory");
        tmpdir_str = r;
    }

    auto cleanup = [&]() {
        std::error_code ec;
        fs::remove_all(tmpdir_str, ec);
    };

    auto stage = [&](const std::string& rel, const std::string& label) {
        std::error_code ec;
        fs::path src = fs::path(esp) / rel;
        fs::path dst = fs::path(tmpdir_str) / rel;
        if (fs::is_regular_file(src, ec)) {
            fs::create_directories(dst.parent_path(), ec);
            fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
            if (ec) rep.warn("failed to copy " + label + ": " + ec.message());
            else std::cout << "  backed up " << label << "\n";
        } else if (fs::is_directory(src, ec)) {
            fs::copy(src, dst,
                     fs::copy_options::overwrite_existing |
                     fs::copy_options::recursive, ec);
            if (ec) rep.warn("failed to copy " + label + ": " + ec.message());
            else std::cout << "  backed up " << label << "\n";
        }
    };

    // ForeB payload
    stage("EFI/forb", "EFI/forb (ForeB loader + assets)");
    stage("forebo", "forebo (ForeB config + icons)");

    // Known bootloader configs
    stage("grub/grub.cfg", "grub/grub.cfg");
    stage("limine.cfg", "limine.cfg");
    stage("limine.conf", "limine.conf");
    stage("loader/loader.conf", "loader/loader.conf (systemd-boot)");
    stage("EFI/refind/refind.conf", "EFI/refind/refind.conf");
    stage("EFI/CLOVER/config.plist", "EFI/CLOVER/config.plist");
    stage("syslinux/syslinux.cfg", "syslinux/syslinux.cfg");

    // NVRAM dump
    if (which("efibootmgr")) {
        auto out = run_cmd({"efibootmgr", "-v"});
        if (out) {
            fs::path nvram_file = fs::path(tmpdir_str) / "nvram-efibootmgr.txt";
            atomic_write(nvram_file.string(), *out);
            std::cout << "  backed up NVRAM entries (efibootmgr -v)\n";
        }
    }

    // Create tar.gz
    std::string tar_cmd = "tar czf " + outpath + " -C " + tmpdir_str + " .";
    std::cout << "\nCreating archive...\n";
    int rc = std::system(tar_cmd.c_str());
    cleanup();
    if (rc != 0) return die("tar failed (exit " + std::to_string(rc) + ")");
    std::cout << "Backup saved to " << outpath << "\n";
    return 0;
}

// ===========================================================================
//  export
// ===========================================================================

static std::string sanitize_label(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\'' || c == '\\') out += '_';
        else out += c;
    }
    return out;
}

static std::string entry_title_or_fallback(const OutEntry& e, size_t idx) {
    if (!e.title.empty()) return e.title;
    if (e.type == "linux") return "Linux " + std::to_string(idx);
    if (e.type == "forest") return "ForeB " + std::to_string(idx);
    if (e.type == "chainload") return "Chainload " + std::to_string(idx);
    return "Entry " + std::to_string(idx);
}

std::string export_to_grub(const ParsedConfig& cfg,
                           const std::vector<OutNode>& roots) {
    std::ostringstream os;
    int timeout = cfg.timeout.value_or(3);
    os << "set timeout=" << timeout << "\n"
       << "set default=0\n\n";

    auto entries = flatten_entries(roots);
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& [entry, path] = entries[i];
        std::string title = entry_title_or_fallback(*entry, i);
        os << "menuentry \"" << sanitize_label(title) << "\" {\n";
        if (entry->type == "linux") {
            os << "    linux " << entry->vmlinuz;
            if (!entry->cmdline.empty())
                os << " " << entry->cmdline;
            os << "\n";
            if (!entry->initrd.empty())
                os << "    initrd " << entry->initrd << "\n";
        } else if (entry->type == "forest") {
            os << "    linux " << entry->kernel << "\n";
            for (const auto& m : entry->modules)
                os << "    module " << m << "\n";
        } else if (entry->type == "chainload" && !entry->chain.empty()) {
            os << "    chainloader " << entry->chain << "\n";
        }
        os << "}\n\n";
    }
    return os.str();
}

std::string export_to_limine(const ParsedConfig& cfg,
                             const std::vector<OutNode>& roots) {
    std::ostringstream os;
    int timeout = cfg.timeout.value_or(5);
    os << "TIMEOUT=" << timeout << "\n\n";

    auto entries = flatten_entries(roots);
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& [entry, path] = entries[i];
        std::string title = entry_title_or_fallback(*entry, i);
        os << ":" << title << "\n";
        if (entry->type == "linux") {
            os << "    PROTOCOL=linux\n";
            os << "    KERNEL_PATH=boot():/" << entry->vmlinuz << "\n";
            if (!entry->cmdline.empty())
                os << "    CMDLINE=" << entry->cmdline << "\n";
            if (!entry->initrd.empty())
                os << "    MODULE_PATH=boot():/" << entry->initrd << "\n";
        } else if (entry->type == "forest") {
            os << "    PROTOCOL=linux\n";
            os << "    KERNEL_PATH=boot():/" << entry->kernel << "\n";
            for (const auto& m : entry->modules)
                os << "    MODULE_PATH=boot():/" << m << "\n";
        } else if (entry->type == "chainload" && !entry->chain.empty()) {
            os << "    PROTOCOL=chainload\n";
            os << "    PATH=boot():/" << entry->chain << "\n";
        }
        os << "\n";
    }
    return os.str();
}

std::string export_to_systemd(const ParsedConfig& cfg,
                              const std::vector<OutNode>& roots) {
    std::ostringstream os;
    auto entries = flatten_entries(roots);
    std::string version = "linux";
    if (!entries.empty() && entries[0].first->type == "forest")
        version = "foreb";

    os << "default " << version << "\n";
    if (cfg.timeout.has_value())
        os << "timeout " << *cfg.timeout << "\n";
    os << "editor no\n\n";

    os << "# Generated forebo.cfg entries:\n";
    os << "# Each entry should be saved as a separate .conf file in the\n";
    os << "# loader/entries/ directory.\n\n";

    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& [entry, path] = entries[i];
        std::string title = entry_title_or_fallback(*entry, i);
        os << "title " << title << "\n";
        if (entry->type == "linux") {
            os << "linux " << entry->vmlinuz << "\n";
            if (!entry->initrd.empty())
                os << "initrd " << entry->initrd << "\n";
            if (!entry->cmdline.empty())
                os << "options " << entry->cmdline << "\n";
        } else if (entry->type == "forest") {
            os << "linux " << entry->kernel << "\n";
            std::string mods;
            for (const auto& m : entry->modules) {
                if (!mods.empty()) mods += " ";
                mods += m;
            }
            if (!mods.empty())
                os << "options " << mods << "\n";
        } else if (entry->type == "chainload" && !entry->chain.empty()) {
            os << "efi " << entry->chain << "\n";
        }
        os << "\n";
    }
    return os.str();
}

std::string export_to_syslinux(const ParsedConfig& cfg,
                               const std::vector<OutNode>& roots) {
    std::ostringstream os;
    int timeout = cfg.timeout.value_or(5);
    os << "TIMEOUT " << (timeout * 10) << "\n\n";

    auto entries = flatten_entries(roots);
    if (!entries.empty()) {
        std::string default_title =
            entry_title_or_fallback(*entries[0].first, 0);
        os << "DEFAULT " << sanitize_label(default_title) << "\n\n";
    }

    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& [entry, path] = entries[i];
        std::string title = entry_title_or_fallback(*entry, i);
        std::string label = sanitize_label(title);
        std::transform(label.begin(), label.end(), label.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        os << "LABEL " << label << "\n";
        os << "    MENU LABEL " << title << "\n";
        if (entry->type == "linux") {
            os << "    LINUX " << entry->vmlinuz << "\n";
            if (!entry->cmdline.empty())
                os << "    APPEND " << entry->cmdline << "\n";
            if (!entry->initrd.empty())
                os << "    INITRD " << entry->initrd << "\n";
        } else if (entry->type == "forest") {
            os << "    LINUX " << entry->kernel << "\n";
            std::string mods;
            for (const auto& m : entry->modules) {
                if (!mods.empty()) mods += " ";
                mods += m;
            }
            if (!mods.empty())
                os << "    APPEND " << mods << "\n";
        } else if (entry->type == "chainload" && !entry->chain.empty()) {
            os << "    COM32 " << entry->chain << "\n";
        }
        os << "\n";
    }
    return os.str();
}

int cmd_export(const Args& args, Reporter& rep) {
    if (args.export_format.empty()) {
        std::cerr << TOOL << ": error: export requires --format\n";
        return 2;
    }
    if (args.output.empty()) {
        std::cerr << TOOL << ": error: export requires --output\n";
        return 2;
    }

    std::string fmt = lower(args.export_format);
    if (fmt != "grub" && fmt != "limine" && fmt != "systemd-boot" &&
        fmt != "syslinux") {
        std::cerr << TOOL << ": error: unknown export format: " << fmt
                  << "\n  supported formats: grub, limine, systemd-boot, syslinux\n";
        return 2;
    }

    auto result = build_config(args, rep);
    if (!result) return 1;

    std::string text;
    if (fmt == "grub")
        text = export_to_grub(result->parsed, result->roots);
    else if (fmt == "limine")
        text = export_to_limine(result->parsed, result->roots);
    else if (fmt == "systemd-boot")
        text = export_to_systemd(result->parsed, result->roots);
    else if (fmt == "syslinux")
        text = export_to_syslinux(result->parsed, result->roots);

    if (fmt == "systemd-boot") {
        fs::path dir(args.output);
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) {
            std::cerr << TOOL << ": error: cannot create " << args.output
                      << ": " << ec.message() << "\n";
            return 1;
        }

        auto entries = flatten_entries(result->roots);
        for (size_t i = 0; i < entries.size(); ++i) {
            const auto& [entry, path] = entries[i];
            std::string title = entry_title_or_fallback(*entry, i);
            std::string safe = sanitize_label(title);
            std::transform(safe.begin(), safe.end(), safe.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            // Replace spaces with dashes
            std::replace(safe.begin(), safe.end(), ' ', '-');
            std::string filename = safe + ".conf";
            fs::path filepath = dir / filename;

            std::ostringstream entry_os;
            entry_os << "title " << title << "\n";
            if (entry->type == "linux") {
                entry_os << "linux " << entry->vmlinuz << "\n";
                if (!entry->initrd.empty())
                    entry_os << "initrd " << entry->initrd << "\n";
                if (!entry->cmdline.empty())
                    entry_os << "options " << entry->cmdline << "\n";
            } else if (entry->type == "forest") {
                entry_os << "linux " << entry->kernel << "\n";
                std::string mods;
                for (const auto& m : entry->modules) {
                    if (!mods.empty()) mods += " ";
                    mods += m;
                }
                if (!mods.empty())
                    entry_os << "options " << mods << "\n";
            } else if (entry->type == "chainload" && !entry->chain.empty()) {
                entry_os << "efi " << entry->chain << "\n";
            }

            atomic_write(filepath.string(), entry_os.str());
            std::cout << "wrote " << filepath.string() << "\n";
        }
    } else {
        atomic_write(args.output, text);
        std::cout << "wrote " << args.output << " (" << fmt << " format)\n";
    }

    return 0;
}

// ===========================================================================
//  migrate
// ===========================================================================
int cmd_migrate(const Args& args, Reporter& rep) {
    bool dry = args.dry_run;

    // Determine scan directory
    std::string scan_dir = args.source;
    if (scan_dir.empty()) {
        auto d = detect_esp();
        if (!d) {
            die("no ESP found: none of /boot, /boot/efi, /efi is a vfat mount "
                "(use --source PATH or --esp PATH)");
            return 1;
        }
        scan_dir = *d;
        rep.note("auto-detected ESP at " + scan_dir);
    }

    std::error_code ec;
    if (!fs::is_directory(scan_dir, ec)) {
        die("source directory not found: " + scan_dir);
        return 1;
    }

    // Detect bootloaders
    auto detected = detect_bootloader(scan_dir);

    if (detected.empty()) {
        std::cout << "No bootloader configs found in " << scan_dir << "\n";
        std::cout << "Searched for: GRUB, Limine, systemd-boot, rEFInd, "
                     "Clover, syslinux, ZBM\n";
        return 1;
    }

    // Display detection results
    std::cout << "Detected " << detected.size() << " bootloader(s) in "
              << scan_dir << ":\n\n";
    for (size_t i = 0; i < detected.size(); ++i) {
        const auto& b = detected[i];
        int pct = static_cast<int>(b.confidence * 100);
        std::cout << "  [" << (i + 1) << "] " << b.name;
        if (i == 0) std::cout << " (best match)";
        std::cout << " - confidence " << pct << "%\n";
        std::cout << "      config: " << b.config_path << "\n";
        if (!b.features.empty()) {
            std::cout << "      migratable: " << join(", ", b.features) << "\n";
        }
        if (!b.warnings.empty()) {
            std::cout << "      warnings:\n";
            for (const auto& w : b.warnings)
                std::cout << "        - " << w << "\n";
        }
        std::cout << "      migration: " << b.migration_path << "\n\n";
    }

    // Pick the best non-foreb source
    const BootloaderInfo* source = nullptr;
    for (const auto& b : detected) {
        if (b.name != "foreb") {
            source = &b;
            break;
        }
    }

    if (!source) {
        std::cout << "ForeB is already the only bootloader; nothing to "
                     "migrate.\n";
        return 0;
    }

    if (source->features.empty()) {
        std::cout << "Cannot migrate from " << source->name
                  << ": no migratable features.\n";
        return 1;
    }

    // If the source parser is not yet implemented, inform and exit
    if (source->name == "refind" || source->name == "clover" ||
        source->name == "zbm") {
        std::cout << "Migration from " << source->name
                  << " is not yet implemented.\n";
        std::cout << "You can manually create forebo.cfg or use:\n";
        std::cout << "  forb-install install --config <your-config>\n";
        return 1;
    }

    // Determine the ESP for parsing
    std::string esp = args.esp;
    if (esp.empty()) {
        auto d = detect_esp();
        if (d) esp = *d;
    }
    if (esp.empty()) esp = scan_dir;

    // Attempt to parse and translate
    std::cout << "Migrating from " << source->name << "...\n\n";

    EspContext ctx(esp, rep);
    ParsedConfig parsed;
    try {
        parsed = parse_source(source->name, source->config_path, esp, ctx, rep);
    } catch (const std::exception& e) {
        die("cannot parse " + source->config_path + ": " + e.what());
        return 1;
    }

    // Build the config
    std::vector<OutNode> roots =
        cap_entries(parsed.roots, args.max_entries, rep);
    for (auto& [e, path] : flatten_entries(roots)) {
        (void)path;
        validate_entry(*e, rep);
    }
    std::string default_str =
        resolve_default(parsed, roots, args.default_entry, rep);
    auto [background, wallpaper_job] = prepare_wallpaper(parsed, ctx, rep);
    std::string cfg_text = emit_config(parsed, roots, default_str, background,
                                       !args.no_extras);

    int n_entries = static_cast<int>(flatten_entries(roots).size());
    int n_submenus = count_submenus(roots);

    // Determine output path
    std::string outpath = args.output;
    if (outpath.empty()) {
        // Default: write to ESP forebo/forebo.cfg
        if (!esp.empty()) {
            outpath = (fs::path(esp) / "forebo" / "forebo.cfg").string();
        } else {
            outpath = "forebo.cfg";
        }
    }

    // Backup if requested
    if (args.backup && !dry) {
        std::error_code bec;
        if (fs::is_regular_file(outpath, bec)) {
            std::string bak = outpath + ".bak";
            std::cout << "backing up " << outpath << " -> " << bak << "\n";
            fs::copy_file(outpath, bak, fs::copy_options::overwrite_existing,
                          bec);
            if (bec) {
                rep.warn("backup failed: " + bec.message());
            }
        }
    }

    // Write or print
    if (dry) {
        std::cout << "[dry-run] would write forebo.cfg to " << outpath << "\n";
        std::cout << "\n--- forebo.cfg ---\n";
        std::cout << cfg_text;
        std::cout << "--- end ---\n";
    } else {
        try {
            fs::create_directories(fs::path(outpath).parent_path(), ec);
            atomic_write(outpath, cfg_text);
        } catch (const std::exception& e) {
            die("cannot write " + outpath + ": " + e.what());
            return 1;
        }
    }

    // Summary
    std::cout << "\nMigration summary:\n";
    std::cout << "  source:      " << source->name << " (" << source->config_path
              << ")\n";
    std::cout << "  confidence:  " << static_cast<int>(source->confidence * 100)
              << "%\n";
    std::cout << "  output:      " << outpath;
    if (dry) std::cout << " (dry-run)";
    std::cout << "\n";
    std::cout << "  entries:     " << n_entries << " (+"
              << (args.no_extras ? 0 : static_cast<int>(EXTRAS.size()))
              << " utility)\n";
    std::cout << "  submenus:    " << n_submenus << "\n";
    if (parsed.timeout.has_value())
        std::cout << "  timeout:     " << *parsed.timeout << "s\n";
    if (parsed.remember_last)
        std::cout << "  remember_last: yes\n";
    if (background)
        std::cout << "  wallpaper:   migrated\n";

    if (!source->warnings.empty()) {
        std::cout << "\n  Review the generated config - some features from "
                     << source->name << " may need manual adjustment.\n";
    }

    std::cout << "\nNext steps:\n";
    std::cout << "  1. Review " << outpath << "\n";
    std::cout << "  2. Run: forb-install install --dry-run\n";
    std::cout << "  3. Run: forb-install install\n";
    return 0;
}

}  // namespace forb
