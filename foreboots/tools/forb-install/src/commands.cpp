// commands.cpp - build pipeline + scan/generate/install commands.
#include "forb/forb.hpp"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <regex>
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
    return "limine";
}

static ParsedConfig parse_source(const std::string& kind,
                                 const std::string& path,
                                 const std::string& esp, EspContext& ctx,
                                 Reporter& rep) {
    if (kind == "limine") return parse_limine(path, ctx, rep);
    if (kind == "grub") return parse_grub(path, ctx, rep);
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
                          Reporter& rep) {
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
    if (num.has_value()) {
        std::cout << prefix << "UEFI boot entry for ForeB already exists (Boot"
                  << *num << "); skipping creation\n";
    } else {
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
        nvram_install(esp, args.make_default, dry, rep);

    // summary
    std::cout << "\nForeB install summary:\n";
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

}  // namespace forb
