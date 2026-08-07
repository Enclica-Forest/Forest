// parsers.cpp - Limine, GRUB, systemd-boot parsers/translators.
#include "forb/forb.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

#include <filesystem>

namespace fs = std::filesystem;

namespace forb {

// helpers over a key/value map -------------------------------------------------
static bool has(const std::map<std::string, std::string>& m,
                const std::string& k) {
    return m.count(k) > 0;
}
static std::string get(const std::map<std::string, std::string>& m,
                       const std::string& k) {
    auto it = m.find(k);
    return it == m.end() ? std::string() : it->second;
}
// Python `a or b`: first non-empty.
static std::string first_key(const std::map<std::string, std::string>& m,
                             const std::string& a, const std::string& b) {
    std::string va = get(m, a);
    if (!va.empty()) return va;
    return get(m, b);
}

// ===========================================================================
//  Limine path resolution
// ===========================================================================
std::string resolve_limine_path(const std::string& value, EspContext& ctx,
                                Reporter& rep, const std::string& where) {
    static const std::regex scheme_re(R"(^(\w+)\(([^)]*)\):(.*)$)");
    static const std::regex hash_re("#[0-9a-fA-F]{32,}$");
    std::string v = strip(value);
    std::smatch m;
    if (std::regex_match(v, m, scheme_re)) {
        std::string scheme = lower(m[1].str());
        std::string arg = strip(m[2].str());
        std::string rest = m[3].str();
        if (scheme == "boot") {
            // pass-through
        } else if (scheme == "guid" || scheme == "uuid") {
            if (!arg.empty()) {
                auto match = ctx.match_uuid(arg);
                if (match.has_value() && *match == false) {
                    rep.warn(where + "path '" + v + "' refers to partition " +
                             arg + " which is not the ESP; ForeB can only "
                             "boot files from the ESP - copy the file onto "
                             "the ESP");
                } else if (!match.has_value()) {
                    rep.note(where + "cannot verify partition " + arg +
                             " for path '" + v + "' (offline); assuming it is "
                             "the ESP");
                }
            }
        } else if (scheme == "hdd" || scheme == "cd") {
            rep.warn(where + "unsupported scheme " + scheme + "() in path '" +
                     v + "'; keeping the path best-effort");
        } else {
            rep.warn(where + "unknown scheme " + scheme + "() in path '" + v +
                     "'; keeping the path best-effort");
        }
        v = rest;
    }
    v = std::regex_replace(v, hash_re, "");
    return normalize_esp_path(v);
}

// ===========================================================================
//  Limine parser + translator
// ===========================================================================
static const std::set<std::string> LIMINE_KNOWN_GLOBALS = {
    "timeout", "default_entry", "remember_last_entry", "wallpaper",
    "interface_branding", "interface_resolution", "interface_branding_color",
    "verbose", "hash_mismatch_panic", "randomize_memory", "max_resolution",
    "backdrop", "term_font", "term_font_scale", "term_margin",
    "term_margin_gradient", "graphics", "quiet",
};

ParsedConfig parse_limine(const std::string& path, EspContext& ctx,
                          Reporter& rep) {
    std::string text = read_text(path);
    ParsedConfig cfg;
    cfg.kind = "limine";
    cfg.source_path = path;

    std::vector<std::shared_ptr<Node>> roots;
    std::vector<std::shared_ptr<Node>> stack;
    std::map<std::string, std::string> globals_;
    // preserve global insertion order
    std::vector<std::string> global_order;

    static const std::regex header_re(R"(^(/+)\+?(.*)$)");
    static const std::regex kv_re(R"(^([A-Za-z0-9_]+)\s*:\s*(.*)$)");

    int lineno = 0;
    for (const std::string& raw : splitlines(text)) {
        ++lineno;
        std::string line = strip(raw);
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '/') {
            std::smatch m;
            if (!std::regex_match(line, m, header_re)) {
                rep.note("line " + std::to_string(lineno) +
                         ": malformed header ignored: '" + line + "'");
                continue;
            }
            int depth = static_cast<int>(m[1].str().size());
            std::string name = strip(m[2].str());
            auto node = std::make_shared<Node>();
            node->name = name;
            node->depth = depth;
            node->line = lineno;
            while (!stack.empty() && stack.back()->depth >= depth)
                stack.pop_back();
            while (!stack.empty() && stack.back()->is_entry())
                stack.pop_back();
            if (!stack.empty())
                stack.back()->children.push_back(node);
            else
                roots.push_back(node);
            stack.push_back(node);
            continue;
        }
        std::smatch m;
        if (!std::regex_match(line, m, kv_re)) {
            rep.note("line " + std::to_string(lineno) +
                     ": unrecognized line ignored: '" + line + "'");
            continue;
        }
        std::string key = lower(m[1].str());
        std::string value = strip(m[2].str());
        if (key == "comment") continue;
        if (!stack.empty()) {
            auto node = stack.back();
            if (key == "module_path") {
                node->module_path.push_back(value);
            } else {
                node->keys[key] = value;
            }
        } else {
            if (!globals_.count(key)) global_order.push_back(key);
            globals_[key] = value;
        }
    }

    // ---- globals -----------------------------------------------------------
    for (const std::string& key : global_order) {
        const std::string& value = globals_[key];
        if (key == "timeout") {
            try {
                cfg.timeout = static_cast<int>(std::lround(std::stod(value)));
            } catch (...) {
                rep.note("invalid timeout '" + value + "' ignored");
            }
        } else if (key == "default_entry") {
            try {
                size_t pos = 0;
                int iv = std::stoi(value, &pos);
                if (pos != value.size()) throw std::invalid_argument("");
                cfg.def.kind = DefaultSpec::Index;
                cfg.def.index = iv;
            } catch (...) {
                rep.note("invalid default_entry '" + value + "' ignored");
            }
        } else if (key == "remember_last_entry") {
            std::string lv = lower(value);
            cfg.remember_last =
                (lv == "yes" || lv == "1" || lv == "true" || lv == "on");
        } else if (key == "wallpaper") {
            if (!value.empty()) {
                cfg.wallpaper_raw = value;
                cfg.wallpaper =
                    resolve_limine_path(value, ctx, rep, "wallpaper: ");
            }
        } else if (LIMINE_KNOWN_GLOBALS.count(key) ||
                   starts_with(key, "term_")) {
            rep.note("global '" + key + "' ignored (cosmetic/unsupported)");
        } else {
            rep.note("unknown global '" + key + "' ignored");
        }
    }

    // ---- translate nodes ----------------------------------------------------
    std::function<std::optional<OutNode>(const std::shared_ptr<Node>&)>
        translate = [&](const std::shared_ptr<Node>& node)
        -> std::optional<OutNode> {
        std::string where = "line " + std::to_string(node->line) + ": ";
        if (!node->is_entry()) {
            std::vector<OutNode> children;
            for (auto& c : node->children) {
                auto out = translate(c);
                if (out) children.push_back(*out);
            }
            if (children.empty() && node->keys.empty()) {
                rep.note(where + "group '" + node->name +
                         "' has no children and no keys; dropped");
                return std::nullopt;
            }
            auto g = std::make_shared<OutGroup>();
            g->title = sanitize_title(node->name);
            g->icon = guess_icon(node->name);
            g->children = std::move(children);
            return make_group(g);
        }

        const auto& keys = node->keys;
        std::string proto = lower(strip(get(keys, "protocol")));
        std::string title = sanitize_title(node->name);

        auto rp = [&](const std::string& v) {
            return resolve_limine_path(v, ctx, rep, where);
        };
        auto cmdline = [&]() -> std::string {
            std::string c;
            if (has(keys, "cmdline")) c = get(keys, "cmdline");
            else if (has(keys, "kernel_cmdline")) c = get(keys, "kernel_cmdline");
            return strip(c);
        };
        auto modules = [&]() -> std::vector<std::string> {
            std::vector<std::string> out;
            for (const auto& mp : node->module_path) out.push_back(rp(mp));
            return out;
        };

        std::shared_ptr<OutEntry> entry;
        if (proto == "linux") {
            std::string kpath = first_key(keys, "kernel_path", "path");
            if (kpath.empty()) {
                rep.warn(where + "entry '" + node->name +
                         "': protocol=linux but no path; skipped");
            } else {
                auto mods = modules();
                std::vector<std::string> extras(
                    mods.begin() + (mods.empty() ? 0 : 1), mods.end());
                for (const auto& e : extras)
                    rep.warn(where + "entry '" + node->name + "': extra "
                             "module " + e + " is unsupported for type=linux; "
                             "emitted as a comment");
                entry = std::make_shared<OutEntry>();
                entry->title = title;
                entry->type = "linux";
                entry->vmlinuz = rp(kpath);
                entry->initrd = mods.empty() ? "" : mods[0];
                entry->extra_comments = extras;
                entry->cmdline = cmdline();
            }
        } else if (proto == "multiboot" || proto == "multiboot1" ||
                   proto == "multiboot2") {
            if (proto == "multiboot2")
                rep.warn(where + "entry '" + node->name + "': ForeB is "
                         "multiboot1-only; a multiboot2 kernel may not boot");
            std::string kpath = first_key(keys, "kernel_path", "path");
            if (kpath.empty()) {
                rep.warn(where + "entry '" + node->name + "': protocol=" +
                         proto + " but no kernel path; skipped");
            } else {
                entry = std::make_shared<OutEntry>();
                entry->title = title;
                entry->type = "forest";
                entry->kernel = rp(kpath);
                entry->modules = modules();
                entry->cmdline = cmdline();
            }
        } else if (proto == "efi_chainload") {
            std::string img = first_key(keys, "image_path", "path");
            if (img.empty()) {
                rep.warn(where + "entry '" + node->name +
                         "': efi_chainload without image_path; skipped");
            } else {
                entry = std::make_shared<OutEntry>();
                entry->title = title;
                entry->type = "chainload";
                entry->chain = rp(img);
                entry->cmdline = cmdline();
            }
        } else if (proto == "efi") {
            std::string p = get(keys, "path");
            if (p.empty()) {
                rep.warn(where + "entry '" + node->name +
                         "': protocol=efi but no path; skipped");
            } else {
                entry = std::make_shared<OutEntry>();
                entry->title = title;
                entry->type = "chainload";
                entry->chain = rp(p);
                entry->cmdline = cmdline();
            }
        } else if (proto == "limine") {
            std::string p = get(keys, "path");
            if (p.empty()) {
                rep.warn(where + "entry '" + node->name +
                         "': protocol=limine but no path; skipped");
            } else {
                entry = std::make_shared<OutEntry>();
                entry->title = title;
                entry->type = "chainload";
                entry->chain = rp(p);
                entry->cmdline = cmdline();
            }
        } else if (proto == "chainload") {
            rep.warn(where + "entry '" + node->name + "': legacy BIOS sector "
                     "chainload is unsupported on UEFI ForeB; skipped");
        } else {
            if (!proto.empty())
                rep.note(where + "entry '" + node->name + "': unknown protocol "
                         "'" + proto + "'; inferring type from paths");
            std::string img = get(keys, "image_path");
            std::string p = get(keys, "path");
            if (!img.empty() || (!p.empty() && ends_with(lower(p), ".efi"))) {
                entry = std::make_shared<OutEntry>();
                entry->title = title;
                entry->type = "chainload";
                entry->chain = rp(!img.empty() ? img : p);
                entry->cmdline = cmdline();
            } else if (!p.empty()) {
                auto mods = modules();
                std::vector<std::string> extras(
                    mods.begin() + (mods.empty() ? 0 : 1), mods.end());
                entry = std::make_shared<OutEntry>();
                entry->title = title;
                entry->type = "linux";
                entry->vmlinuz = rp(p);
                entry->initrd = mods.empty() ? "" : mods[0];
                entry->extra_comments = extras;
                entry->cmdline = cmdline();
            } else {
                rep.warn(where + "entry '" + node->name +
                         "': no bootable path found; skipped");
            }
        }
        if (entry) {
            std::string base = !entry->vmlinuz.empty() ? entry->vmlinuz
                             : !entry->kernel.empty()  ? entry->kernel
                                                       : entry->chain;
            entry->icon =
                guess_icon(node->name + " " + base_name(base), entry->type);
            cfg.index_entries.push_back(entry);
            return make_entry(entry);
        }
        return std::nullopt;
    };

    for (auto& n : roots) {
        auto out = translate(n);
        if (out) cfg.roots.push_back(*out);
    }
    return cfg;
}

// ===========================================================================
//  GRUB parser (best-effort)
// ===========================================================================
struct GrubFrame {
    std::string kind;   // menuentry|submenu
    std::string title;
    std::vector<std::string> classes;
    int line = 0;
    // directives
    std::string linux_d;
    bool has_linux = false;
    std::vector<std::string> initrd_d;
    std::string chainloader_d;
    bool has_chainloader = false;
    std::string multiboot_d;
    bool has_multiboot = false;
    std::vector<std::string> module_d;
    std::vector<OutNode> children;
};

static std::string grub_clean_path(const std::string& p_in, Reporter& rep,
                                   const std::string& where) {
    std::string orig = p_in;
    std::string p = p_in;
    p = std::regex_replace(p, std::regex(R"(^\([^)]*\))"), "");
    p = std::regex_replace(p, std::regex(R"(^\$\{?root\}?)"), "");
    p = strip(p);
    if (orig != p || p.empty() || p[0] != '/')
        rep.warn(where + "path '" + orig + "' does not look ESP-absolute; "
                 "ForeB can only boot files from the ESP - verify/copy it");
    return normalize_esp_path(p);
}

ParsedConfig parse_grub(const std::string& path, EspContext& /*ctx*/,
                        Reporter& rep) {
    std::string text = read_text(path);
    ParsedConfig cfg;
    cfg.kind = "grub";
    cfg.source_path = path;
    rep.note("grub.cfg parsing is best-effort");

    std::vector<GrubFrame> frames;
    std::optional<GrubFrame> pending;
    std::optional<std::string> set_default, set_timeout;

    auto finalize = [&](GrubFrame& frame) -> std::optional<OutNode> {
        std::string where = "line " + std::to_string(frame.line) + ": ";
        std::string title = sanitize_title(frame.title);
        if (frame.kind == "submenu") {
            if (frame.children.empty()) {
                rep.note(where + "submenu '" + frame.title +
                         "' is empty; dropped");
                return std::nullopt;
            }
            auto g = std::make_shared<OutGroup>();
            g->title = title;
            g->icon = guess_icon(frame.title);
            g->children = std::move(frame.children);
            return make_group(g);
        }
        std::shared_ptr<OutEntry> entry;
        if (frame.has_chainloader) {
            entry = std::make_shared<OutEntry>();
            entry->title = title;
            entry->type = "chainload";
            entry->chain = grub_clean_path(frame.chainloader_d, rep, where);
        } else if (frame.has_linux) {
            std::string linux_line = frame.linux_d;
            // split(None, 1)
            std::string first, restcmd;
            {
                size_t i = 0;
                while (i < linux_line.size() &&
                       std::isspace((unsigned char)linux_line[i])) ++i;
                size_t j = i;
                while (j < linux_line.size() &&
                       !std::isspace((unsigned char)linux_line[j])) ++j;
                first = linux_line.substr(i, j - i);
                while (j < linux_line.size() &&
                       std::isspace((unsigned char)linux_line[j])) ++j;
                restcmd = linux_line.substr(j);
            }
            std::string vmlinuz = grub_clean_path(first, rep, where);
            std::string cmd = strip(restcmd);
            std::vector<std::string> initrds;
            for (const auto& x : frame.initrd_d)
                initrds.push_back(grub_clean_path(x, rep, where));
            std::vector<std::string> extras(
                initrds.begin() + (initrds.empty() ? 0 : 1), initrds.end());
            for (const auto& extra : extras)
                rep.warn(where + "entry '" + frame.title + "': extra initrd " +
                         extra + " is unsupported; emitted as a comment");
            entry = std::make_shared<OutEntry>();
            entry->title = title;
            entry->type = "linux";
            entry->vmlinuz = vmlinuz;
            entry->initrd = initrds.empty() ? "" : initrds[0];
            entry->extra_comments = extras;
            entry->cmdline = cmd;
        } else if (frame.has_multiboot) {
            std::vector<std::string> mods;
            for (const auto& mstr : frame.module_d)
                mods.push_back(grub_clean_path(mstr, rep, where));
            entry = std::make_shared<OutEntry>();
            entry->title = title;
            entry->type = "forest";
            entry->kernel = grub_clean_path(frame.multiboot_d, rep, where);
            entry->modules = mods;
        } else {
            rep.note(where + "menuentry '" + frame.title +
                     "' has no boot directive; skipped");
            return std::nullopt;
        }
        std::string base = !entry->vmlinuz.empty() ? entry->vmlinuz
                         : !entry->kernel.empty()  ? entry->kernel
                                                   : entry->chain;
        std::string hint = frame.title + " " + join(" ", frame.classes) + " " +
                           base_name(base);
        entry->icon = guess_icon(hint, entry->type);
        cfg.index_entries.push_back(entry);
        return make_entry(entry);
    };

    auto close_frame = [&](GrubFrame frame) {
        auto node = finalize(frame);
        if (!node) return;
        if (!frames.empty())
            frames.back().children.push_back(*node);
        else
            cfg.roots.push_back(*node);
    };

    static const std::regex me_re(R"(^(menuentry|submenu)\b(.*)$)");
    static const std::regex title_re(R"(^\s*(["'])(.*?)\1)");
    static const std::regex class_re(R"(--class[=\s]+([^\s]+))");
    static const std::regex content_re(
        R"(^(linux|initrd|chainloader|multiboot|module)\s+(.*)$)");
    static const std::regex ignore_re(
        R"(^(search|set\s+root|insmod|savedefault|recordfail|load_video|echo|sleep|if\b|fi\b|else|elif\b|then|menuentry_id_option|set\s+menuentry))");
    static const std::regex setdef_re(R"(^set\s+(default|timeout)=(.*)$)");

    auto count_ch = [](const std::string& s, char c) {
        int n = 0; for (char ch : s) if (ch == c) ++n; return n;
    };

    int lineno = 0;
    for (const std::string& raw : splitlines(text)) {
        ++lineno;
        std::string line = strip(raw);
        if (line.empty() || line[0] == '#') continue;

        std::smatch m;
        if (std::regex_match(line, m, me_re)) {
            std::string kind = m[1].str();
            std::string rest = m[2].str();
            std::smatch tm;
            if (!std::regex_search(rest, tm, title_re)) {
                rep.warn("line " + std::to_string(lineno) +
                         ": could not parse " + kind + " title; block ignored");
                continue;
            }
            std::string title = tm[2].str();
            std::string after = rest.substr(tm.position(0) + tm.length(0));
            std::vector<std::string> classes;
            for (auto it = std::sregex_iterator(after.begin(), after.end(),
                                                class_re);
                 it != std::sregex_iterator(); ++it)
                classes.push_back((*it)[1].str());
            GrubFrame frame;
            frame.kind = kind;
            frame.title = title;
            frame.classes = classes;
            frame.line = lineno;
            int opens = count_ch(after, '{');
            int closes = count_ch(after, '}');
            if (opens > 0) { frames.push_back(frame); opens -= 1; }
            else pending = frame;
            int delta = opens - closes;
            while (delta < 0 && !frames.empty()) {
                GrubFrame f = frames.back(); frames.pop_back();
                close_frame(f);
                delta += 1;
            }
            continue;
        }

        if (pending.has_value()) {
            if (line.find('{') != std::string::npos) {
                frames.push_back(*pending);
                pending.reset();
                line = strip(line.substr(line.find('{') + 1));
                if (line.empty()) continue;
            } else {
                continue;
            }
        }

        if (!frames.empty() && frames.back().kind == "menuentry") {
            std::string content = line;
            size_t br = content.find('}');
            if (br != std::string::npos) content = content.substr(0, br);
            content = strip(content);
            if (!content.empty() && content.back() == ';')
                content.pop_back();
            content = strip(content);
            if (!content.empty()) {
                std::string where = "line " + std::to_string(lineno) + ": ";
                std::smatch cm;
                if (std::regex_match(content, cm, content_re)) {
                    std::string key = cm[1].str();
                    std::string val = strip(cm[2].str());
                    GrubFrame& d = frames.back();
                    if (key == "linux") {
                        if (!d.has_linux) { d.linux_d = val; d.has_linux = true; }
                    } else if (key == "initrd") {
                        std::istringstream iss(val);
                        std::string tok;
                        while (iss >> tok) d.initrd_d.push_back(tok);
                    } else if (key == "chainloader") {
                        if (!d.has_chainloader) {
                            d.chainloader_d = val; d.has_chainloader = true;
                        }
                    } else if (key == "multiboot") {
                        if (!d.has_multiboot) {
                            d.multiboot_d = val; d.has_multiboot = true;
                        }
                    } else if (key == "module") {
                        std::istringstream iss(val);
                        std::string tok;
                        if (iss >> tok) d.module_d.push_back(tok);
                    }
                } else if (std::regex_search(content, ignore_re)) {
                    // ignored grub machinery
                } else {
                    rep.note(where + "grub directive ignored: '" + content +
                             "'");
                }
            }
        }

        int delta = count_ch(line, '{') - count_ch(line, '}');
        while (delta < 0 && !frames.empty()) {
            GrubFrame f = frames.back(); frames.pop_back();
            close_frame(f);
            delta += 1;
        }
        if (frames.empty() && !pending.has_value()) {
            std::smatch sm;
            if (std::regex_match(line, sm, setdef_re)) {
                std::string val = strip(sm[2].str());
                if (!val.empty() && (val.front() == '"' || val.front() == '\''))
                    val = val.substr(1);
                if (!val.empty() && (val.back() == '"' || val.back() == '\''))
                    val.pop_back();
                if (sm[1].str() == "default") set_default = val;
                else set_timeout = val;
            }
        }
    }
    if (pending.has_value())
        rep.note("line " + std::to_string(pending->line) + ": " +
                 pending->kind + " '" + pending->title +
                 "' never opened its block; dropped");
    while (!frames.empty()) {
        rep.note("line " + std::to_string(frames.back().line) +
                 ": unbalanced braces; " + frames.back().kind + " '" +
                 frames.back().title + "' closed at EOF");
        GrubFrame f = frames.back(); frames.pop_back();
        close_frame(f);
    }

    // ---- globals -----------------------------------------------------------
    if (set_timeout.has_value()) {
        try {
            cfg.timeout = static_cast<int>(std::lround(std::stod(*set_timeout)));
        } catch (...) {
            rep.note("grub timeout '" + *set_timeout + "' ignored");
        }
    }
    if (set_default.has_value()) {
        const std::string& sd = *set_default;
        bool digits = !sd.empty() &&
            std::all_of(sd.begin(), sd.end(),
                        [](char c) { return std::isdigit((unsigned char)c); });
        if (digits) {
            cfg.def.kind = DefaultSpec::Index;
            cfg.def.index = std::stoi(sd) + 1;  // grub is 0-based
        } else {
            std::vector<std::string> segs;
            std::istringstream iss(sd);
            std::string seg;
            while (std::getline(iss, seg, '>')) {
                std::string s = strip(seg);
                if (!s.empty()) segs.push_back(sanitize_title(s));
            }
            if (!segs.empty()) {
                cfg.def.kind = DefaultSpec::Path;
                cfg.def.str = join("/", segs);
            }
        }
    }
    return cfg;
}

// ===========================================================================
//  systemd-boot parser
// ===========================================================================
ParsedConfig parse_systemd_boot(const std::string& esp_dir,
                                const std::string& conf_path, Reporter& rep) {
    ParsedConfig cfg;
    cfg.kind = "systemd-boot";
    cfg.source_path = conf_path;
    std::optional<std::string> default_pat;

    auto split1 = [](const std::string& line)
        -> std::pair<std::string, std::string> {
        size_t i = 0;
        while (i < line.size() && !std::isspace((unsigned char)line[i])) ++i;
        std::string key = line.substr(0, i);
        while (i < line.size() && std::isspace((unsigned char)line[i])) ++i;
        return {key, line.substr(i)};
    };

    int lineno = 0;
    for (const std::string& raw : splitlines(read_text(conf_path))) {
        ++lineno;
        std::string line = strip(raw);
        if (line.empty() || line[0] == '#') continue;
        auto [k, v] = split1(line);
        std::string key = lower(k);
        std::string val = strip(v);
        if (key == "default") {
            default_pat = val;
        } else if (key == "timeout") {
            try {
                cfg.timeout = static_cast<int>(std::lround(std::stod(val)));
            } catch (...) {
                rep.note("line " + std::to_string(lineno) +
                         ": loader.conf timeout '" + val + "' ignored");
            }
        } else {
            rep.note("line " + std::to_string(lineno) + ": loader.conf key '" +
                     key + "' ignored");
        }
    }

    std::string entries_dir = (fs::path(esp_dir) / "loader" / "entries").string();
    std::vector<std::string> names;
    std::error_code ec;
    if (fs::is_directory(entries_dir, ec)) {
        for (const auto& e : fs::directory_iterator(entries_dir, ec)) {
            std::string n = e.path().filename().string();
            if (ends_with(n, ".conf")) names.push_back(n);
        }
        std::sort(names.begin(), names.end());
    } else {
        rep.warn("no loader/entries/*.conf found under " + esp_dir);
    }

    for (const std::string& name : names) {
        std::string p = (fs::path(entries_dir) / name).string();
        std::string title, linux_v, efi_v;
        std::vector<std::string> initrds, options;
        int ln = 0;
        for (const std::string& raw : splitlines(read_text(p))) {
            ++ln;
            std::string line = strip(raw);
            if (line.empty() || line[0] == '#') continue;
            auto [k, v] = split1(line);
            std::string key = lower(k);
            std::string val = strip(v);
            if (key == "title") title = val;
            else if (key == "linux") linux_v = val;
            else if (key == "efi") efi_v = val;
            else if (key == "initrd") { if (!val.empty()) initrds.push_back(val); }
            else if (key == "options") options.push_back(strip(val));
            else rep.note(name + " line " + std::to_string(ln) + ": key '" +
                          key + "' ignored");
        }
        if (title.empty())
            title = ends_with(name, ".conf")
                        ? name.substr(0, name.size() - 5) : name;
        std::string where = name + ": ";
        std::vector<std::string> opt_nonempty;
        for (const auto& o : options) if (!o.empty()) opt_nonempty.push_back(o);
        std::string cmd = strip(join(" ", opt_nonempty));
        std::shared_ptr<OutEntry> entry;
        if (!linux_v.empty()) {
            if (!efi_v.empty())
                rep.note(where + "has both linux= and efi=; using linux=");
            std::vector<std::string> ni;
            for (const auto& i : initrds) ni.push_back(normalize_esp_path(i));
            std::vector<std::string> extras(
                ni.begin() + (ni.empty() ? 0 : 1), ni.end());
            for (const auto& extra : extras)
                rep.warn(where + "entry '" + title + "': extra initrd " +
                         extra + " is unsupported; emitted as a comment");
            entry = std::make_shared<OutEntry>();
            entry->title = sanitize_title(title);
            entry->type = "linux";
            entry->vmlinuz = normalize_esp_path(linux_v);
            entry->initrd = ni.empty() ? "" : ni[0];
            entry->extra_comments = extras;
            entry->cmdline = cmd;
            entry->source_name = name;
        } else if (!efi_v.empty()) {
            entry = std::make_shared<OutEntry>();
            entry->title = sanitize_title(title);
            entry->type = "chainload";
            entry->chain = normalize_esp_path(efi_v);
            entry->cmdline = cmd;
            entry->source_name = name;
        } else {
            rep.warn(where + "entry '" + title +
                     "' has neither linux= nor efi=; skipped");
            continue;
        }
        std::string base = !entry->vmlinuz.empty() ? entry->vmlinuz
                                                   : entry->chain;
        entry->icon = guess_icon(title + " " + base_name(base), entry->type);
        cfg.roots.push_back(make_entry(entry));
        cfg.index_entries.push_back(entry);
    }

    if (default_pat.has_value() && !default_pat->empty()) {
        cfg.def.kind = DefaultSpec::Pattern;
        cfg.def.str = *default_pat;
    }
    return cfg;
}

}  // namespace forb
