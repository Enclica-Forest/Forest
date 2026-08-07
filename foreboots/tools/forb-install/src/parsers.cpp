// parsers.cpp - Limine, GRUB, systemd-boot, syslinux, rEFInd, ZBM
//               parsers/translators.
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
//  GRUB parser
// ===========================================================================

// Variable store for GRUB `set` directives.
struct GrubVars {
    std::map<std::string, std::string> vars;

    void set(const std::string& k, const std::string& v) { vars[k] = v; }
    std::string get(const std::string& k) const {
        auto it = vars.find(k);
        return it == vars.end() ? std::string() : it->second;
    }
    bool has(const std::string& k) const { return vars.count(k) > 0; }

    // Expand $var and ${var} references in text.
    std::string expand(const std::string& text) const {
        static const std::regex var_re(R"(\$[{]?([A-Za-z_][A-Za-z0-9_]*)[}]?)");
        std::string result;
        std::sregex_iterator it(text.begin(), text.end(), var_re), end;
        size_t last = 0;
        for (; it != end; ++it) {
            size_t pos = it->position();
            result += text.substr(last, pos - last);
            std::string name = (*it)[1].str();
            result += get(name);
            last = pos + it->length();
        }
        result += text.substr(last);
        return result;
    }
};

struct GrubFrame {
    std::string kind;   // menuentry|submenu
    std::string title;
    std::vector<std::string> classes;
    std::string id;      // --id value
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
    // track which line the title was on, for warnings
    int title_line = 0;
};

static std::string grub_clean_path(const std::string& p_in, Reporter& rep,
                                   const std::string& where) {
    std::string orig = p_in;
    std::string p = p_in;
    // Strip GRUB device part like (hd0,msdos1)
    p = std::regex_replace(p, std::regex(R"(^\([^)]*\))"), "");
    // Strip $(root) and $root (variable will already be expanded)
    p = std::regex_replace(p, std::regex(R"(^\$\{?root\}?)"), "");
    p = strip(p);
    if (orig != p || p.empty() || p[0] != '/')
        rep.warn(where + "path '" + orig + "' does not look ESP-absolute; "
                 "ForeB can only boot files from the ESP - verify/copy it");
    return normalize_esp_path(p);
}

// Check whether a GRUB `if` condition is "simple enough" to evaluate.
// Returns nullopt if unresolvable.
static std::optional<bool> eval_grub_condition(const std::string& cond,
                                               const GrubVars& vars) {
    std::string c = strip(cond);
    // Negation
    if (starts_with(c, "!")) {
        auto inner = eval_grub_condition(c.substr(1), vars);
        return inner.has_value() ? std::optional<bool>(!*inner) : std::nullopt;
    }
    // Parenthesized condition
    if (c.size() >= 2 && c.front() == '(' && c.back() == ')') {
        return eval_grub_condition(c.substr(1, c.size() - 2), vars);
    }
    // string comparisons: "a" = "b", "a" != "b", "a" == "b"
    static const std::regex cmp_re(
        R"(^\s*["']?(.*?)["']?\s*(=|==|!=|<|>|<=|>=)\s*["']?(.*?)["']?\s*$)");
    std::smatch m;
    if (std::regex_match(c, m, cmp_re)) {
        std::string lhs = vars.expand(strip(m[1].str()));
        std::string op = m[2].str();
        std::string rhs = vars.expand(strip(m[3].str()));
        if (op == "=" || op == "==") return lhs == rhs;
        if (op == "!=") return lhs != rhs;
        if (op == "<")  return lhs < rhs;
        if (op == ">")  return lhs > rhs;
        if (op == "<=") return lhs <= rhs;
        if (op == ">=") return lhs >= rhs;
        return std::nullopt;
    }
    // -n "string" : true if string is non-empty
    if (starts_with(c, "-n")) {
        std::string arg = strip(c.substr(2));
        if (!arg.empty() && (arg.front() == '"' || arg.front() == '\''))
            arg = arg.substr(1);
        if (!arg.empty() && (arg.back() == '"' || arg.back() == '\''))
            arg.pop_back();
        return !vars.expand(arg).empty();
    }
    // -z "string" : true if string is empty
    if (starts_with(c, "-z")) {
        std::string arg = strip(c.substr(2));
        if (!arg.empty() && (arg.front() == '"' || arg.front() == '\''))
            arg = arg.substr(1);
        if (!arg.empty() && (arg.back() == '"' || arg.back() == '\''))
            arg.pop_back();
        return vars.expand(arg).empty();
    }
    // Bare variable test: $var or ${var} -- true if non-empty
    if (starts_with(c, "$")) {
        return !vars.expand(c).empty();
    }
    return std::nullopt;
}

ParsedConfig parse_grub(const std::string& path, EspContext& /*ctx*/,
                        Reporter& rep) {
    std::string text = read_text(path);
    ParsedConfig cfg;
    cfg.kind = "grub";
    cfg.source_path = path;

    GrubVars vars;
    // Pre-set well-known variables
    vars.set("root", "");
    vars.set("prefix", "");

    std::vector<GrubFrame> frames;
    std::optional<GrubFrame> pending;
    std::optional<std::string> set_default, set_timeout;
    std::string saved_entry;

    // Track if/fi nesting. Each entry is {line_number, condition_resolved, condition_met}.
    struct CondFrame { int line; bool resolved; bool met; bool active; };
    std::vector<CondFrame> cond_stack;
    bool in_false_branch = false;  // true when we're inside a false if/elif branch

    // source recursion guard
    std::set<std::string> included_files;

    auto should_skip = [&]() -> bool {
        if (cond_stack.empty()) return false;
        // Skip if any ancestor condition was resolved as false
        for (auto& cf : cond_stack) {
            if (cf.resolved && !cf.met && cf.active) return true;
        }
        return false;
    };

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
        // Use --id if available, else title + classes for icon hint
        std::string id_hint = frame.id.empty() ? "" : frame.id + " ";
        std::string hint = id_hint + frame.title + " " + join(" ", frame.classes) + " " +
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
    static const std::regex id_re(R"(--id[=\s]+([^\s]+))");
    static const std::regex content_re(
        R"(^(linux|linuxefi|initrd|initrdefi|chainloader|multiboot|module|loopback)\s+(.*)$)");
    static const std::regex setdef_re(R"(^set\s+(default|timeout)=(.*)$)");

    auto count_ch = [](const std::string& s, char c) {
        int n = 0; for (char ch : s) if (ch == c) ++n; return n;
    };

    // Process a single file.  Returns false on recursion limit.
    std::function<bool(const std::string&, int)> process_file;
    process_file = [&](const std::string& filepath, int depth) -> bool {
        if (depth > 8) {
            rep.warn("source/configfile recursion too deep (>" +
                     std::to_string(depth) + ") in '" + filepath + "'; skipped");
            return false;
        }
        if (included_files.count(filepath)) {
            rep.note("source/configfile '" + filepath +
                     "' already included; skipping");
            return true;
        }
        included_files.insert(filepath);

        std::string file_text;
        try {
            file_text = read_text(filepath);
        } catch (...) {
            rep.warn("cannot read source/configfile '" + filepath + "'");
            return false;
        }

        int file_lineno = 0;
        for (const std::string& raw : splitlines(file_text)) {
            ++file_lineno;
            std::string line = strip(raw);
            if (line.empty() || line[0] == '#') continue;

            // Expand variables in every line
            line = vars.expand(line);

            // --- set variable ---------------------------------------------------
            {
                std::smatch sm;
                if (std::regex_match(line, sm, setdef_re)) {
                    std::string varname = sm[1].str();
                    std::string val = strip(sm[2].str());
                    // Strip quotes
                    if (!val.empty() && (val.front() == '"' || val.front() == '\''))
                        val = val.substr(1);
                    if (!val.empty() && (val.back() == '"' || val.back() == '\''))
                        val.pop_back();
                    vars.set(varname, val);
                    if (varname == "default") set_default = val;
                    else if (varname == "timeout") set_timeout = val;
                    else if (varname == "saved_entry") saved_entry = val;
                    continue;
                }
            }

            // --- export ---------------------------------------------------------
            if (starts_with(line, "export ")) {
                // export just marks a variable for child configs; we already track all vars
                continue;
            }

            // --- source ---------------------------------------------------------
            if (starts_with(line, "source ") || starts_with(line, "configfile ")) {
                std::string src_path = strip(line.substr(line.find(' ')));
                // Strip quotes
                if (!src_path.empty() && (src_path.front() == '"' || src_path.front() == '\''))
                    src_path = src_path.substr(1);
                if (!src_path.empty() && (src_path.back() == '"' || src_path.back() == '\''))
                    src_path.pop_back();
                src_path = vars.expand(src_path);
                if (!src_path.empty() && src_path[0] == '/') {
                    process_file(src_path, depth + 1);
                } else {
                    rep.note("line " + std::to_string(file_lineno) +
                             ": source/configfile path '" + src_path +
                             "' is not absolute; skipped");
                }
                continue;
            }

            // --- search ---------------------------------------------------------
            if (starts_with(line, "search ")) {
                std::string args = strip(line.substr(7));
                // search --set=root --fs-uuid UUID
                if (args.find("--set=root") != std::string::npos) {
                    // Extract UUID/device from the args
                    static const std::regex uuid_re(
                        R"((--[a-z-]+(?:=[^\s]+)?|(?!--)[^\s]+))");
                    std::string uuid_val;
                    for (std::smatch um;
                         std::regex_search(args, um, uuid_re);
                         args = um.suffix()) {
                        std::string tok = um[0].str();
                        if (tok == "--set=root") continue;
                        if (starts_with(tok, "--set=")) continue;
                        if (tok == "--fs-uuid" || tok == "--label" ||
                            tok == "--hint" || tok == "--no-floppy" ||
                            starts_with(tok, "--")) continue;
                        uuid_val = tok;
                    }
                    if (!uuid_val.empty()) {
                        vars.set("root", uuid_val);
                        rep.note("line " + std::to_string(file_lineno) +
                                 ": search --set=root " + uuid_val +
                                 " (root device changed)");
                    }
                }
                // We don't actually resolve the device, just note it
                continue;
            }

            // --- if / elif / else / fi -----------------------------------------
            if (starts_with(line, "if ")) {
                std::string cond = strip(line.substr(3));
                // Strip 'then' at end
                if (ends_with(cond, " then"))
                    cond = strip(cond.substr(0, cond.size() - 5));
                auto result = eval_grub_condition(cond, vars);
                bool resolved = result.has_value();
                bool met = resolved ? *result : false;
                CondFrame cf{file_lineno, resolved, met, true};
                cond_stack.push_back(cf);
                in_false_branch = !resolved || !met;
                continue;
            }
            if (line == "elif" || starts_with(line, "elif ")) {
                if (!cond_stack.empty()) {
                    auto& top = cond_stack.back();
                    if (top.resolved && top.met) {
                        // Previous branch was true, skip rest
                        top.active = false;
                        in_false_branch = true;
                    } else if (starts_with(line, "elif ")) {
                        std::string cond = strip(line.substr(5));
                        if (ends_with(cond, " then"))
                            cond = strip(cond.substr(0, cond.size() - 5));
                        auto result = eval_grub_condition(cond, vars);
                        if (result.has_value()) {
                            top.resolved = true;
                            top.met = *result;
                            top.active = true;
                            in_false_branch = !*result;
                        } else {
                            top.resolved = false;
                            top.met = false;
                            top.active = true;
                            in_false_branch = true;
                        }
                    }
                }
                continue;
            }
            if (line == "else") {
                if (!cond_stack.empty()) {
                    auto& top = cond_stack.back();
                    if (top.resolved) {
                        top.met = !top.met;
                        top.active = top.met;
                        in_false_branch = !top.met;
                    }
                }
                continue;
            }
            if (line == "fi") {
                if (!cond_stack.empty()) {
                    cond_stack.pop_back();
                    // Restore in_false_branch from remaining stack
                    in_false_branch = false;
                    for (auto it = cond_stack.rbegin(); it != cond_stack.rend(); ++it) {
                        if (it->resolved && !it->met && it->active) {
                            in_false_branch = true;
                            break;
                        }
                    }
                }
                continue;
            }

            // Skip if we're in a false branch
            if (should_skip()) continue;

            // --- menuentry / submenu -------------------------------------------
            std::smatch m;
            if (std::regex_match(line, m, me_re)) {
                std::string kind = m[1].str();
                std::string rest = m[2].str();
                std::smatch tm;
                if (!std::regex_search(rest, tm, title_re)) {
                    rep.warn("line " + std::to_string(file_lineno) +
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
                // --id
                std::string entry_id;
                std::smatch idm;
                if (std::regex_search(after, idm, id_re)) {
                    entry_id = idm[1].str();
                }
                GrubFrame frame;
                frame.kind = kind;
                frame.title = title;
                frame.classes = classes;
                frame.id = entry_id;
                frame.line = file_lineno;
                frame.title_line = file_lineno;
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

            // --- directives inside menuentry/submenu ---------------------------
            if (!frames.empty() && frames.back().kind == "menuentry") {
                std::string content = line;
                size_t br = content.find('}');
                if (br != std::string::npos) content = content.substr(0, br);
                content = strip(content);
                if (!content.empty() && content.back() == ';')
                    content.pop_back();
                content = strip(content);
                if (!content.empty()) {
                    std::string where2 = "line " + std::to_string(file_lineno) + ": ";
                    std::smatch cm;
                    if (std::regex_match(content, cm, content_re)) {
                        std::string key = cm[1].str();
                        std::string val = strip(cm[2].str());
                        GrubFrame& d = frames.back();
                        if (key == "linux" || key == "linuxefi") {
                            if (!d.has_linux) { d.linux_d = val; d.has_linux = true; }
                        } else if (key == "initrd" || key == "initrdefi") {
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
                        } else if (key == "loopback") {
                            rep.note(where2 + "loopback detected; "
                                     "ForeB cannot chain-load loopback "
                                     "images; entry may not boot");
                        }
                    } else if (starts_with(content, "set ")) {
                        // Allow set inside menuentry (e.g., set root=(hd0,1))
                        std::smatch sm;
                        if (std::regex_match(content, sm, setdef_re)) {
                            std::string varname = sm[1].str();
                            std::string val = strip(sm[2].str());
                            if (!val.empty() && (val.front() == '"' || val.front() == '\''))
                                val = val.substr(1);
                            if (!val.empty() && (val.back() == '"' || val.back() == '\''))
                                val.pop_back();
                            vars.set(varname, val);
                        }
                    } else if (starts_with(content, "insmod ") ||
                               starts_with(content, "load_video") ||
                               starts_with(content, "recordfail") ||
                               starts_with(content, "echo ") ||
                               starts_with(content, "sleep ")) {
                        // Ignored grub machinery
                    } else if (starts_with(content, "savedefault")) {
                        // savedefault -- skip
                    } else {
                        rep.note(where2 + "grub directive ignored: '" + content + "'");
                    }
                }
            } else if (!frames.empty() && frames.back().kind == "submenu") {
                // Directives at submenu level that aren't menuentry/submenu
                // (e.g., set root=, insmod) -- ignore silently
            }

            // --- brace balancing ------------------------------------------------
            int delta = count_ch(line, '{') - count_ch(line, '}');
            while (delta < 0 && !frames.empty()) {
                GrubFrame f = frames.back(); frames.pop_back();
                close_frame(f);
                delta += 1;
            }
        }

        // Close any frames still open at end of this file
        // (but don't close frames from the parent -- only those we opened)
        // We can't easily track this without a frame-depth counter, so we leave
        // it to the outer loop's EOF handling.
        return true;
    };

    bool ok = process_file(path, 0);
    (void)ok;

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

    // Resolve default: check saved_entry first, then set default
    std::string effective_default;
    if (!saved_entry.empty()) {
        effective_default = saved_entry;
    } else if (set_default.has_value()) {
        effective_default = *set_default;
    }

    if (!effective_default.empty()) {
        const std::string& sd = effective_default;
        bool digits = !sd.empty() &&
            std::all_of(sd.begin(), sd.end(),
                        [](char c) { return std::isdigit((unsigned char)c); });
        if (digits) {
            cfg.def.kind = DefaultSpec::Index;
            cfg.def.index = std::stoi(sd) + 1;  // grub is 0-based
        } else {
            // Try title-based matching (e.g., "Ubuntu" or submenu>entry)
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

    // Emit notes for unresolvable features
    if (!cond_stack.empty()) {
        rep.warn("unbalanced if/fi; " + std::to_string(cond_stack.size()) +
                 " unclosed conditional(s)");
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

// ===========================================================================
//  Syslinux / Isolinux parser
// ===========================================================================
ParsedConfig parse_syslinux(const std::string& path, EspContext& /*ctx*/,
                            Reporter& rep) {
    ParsedConfig cfg;
    cfg.kind = "syslinux";
    cfg.source_path = path;

    // ---- recursive file reader (handles INCLUDE) --------------------------
    std::function<std::string(const std::string&, int)> read_recursive =
        [&](const std::string& p, int depth) -> std::string {
        if (depth > 8) {
            rep.warn("syslinux INCLUDE depth limit reached at '" + p +
                     "'; ignoring");
            return "";
        }
        std::string text;
        try {
            text = read_text(p);
        } catch (const std::exception& e) {
            rep.warn("syslinux INCLUDE: cannot read '" + p + "': " + e.what());
            return "";
        }
        // process INCLUDE directives inline so recursive includes work
        std::string result;
        for (const std::string& raw : splitlines(text)) {
            std::string line = strip(raw);
            if (starts_with(lower(line), "include ")) {
                std::string inc_path = strip(line.substr(8));
                if (!inc_path.empty()) {
                    // resolve relative to the directory of the including file
                    if (!inc_path.empty() && inc_path[0] != '/') {
                        std::string dir =
                            fs::path(p).parent_path().string();
                        inc_path = (fs::path(dir) / inc_path).string();
                    }
                    result += read_recursive(inc_path, depth + 1);
                    result += "\n";
                }
            } else {
                result += raw;
                result += "\n";
            }
        }
        return result;
    };

    std::string text = read_recursive(path, 0);

    // ---- state for LABEL blocks -------------------------------------------
    struct SysLabel {
        std::string name;           // raw LABEL name (for DEFAULT matching)
        std::string menu_label;     // MENU LABEL value
        std::string kernel;         // KERNEL directive value
        std::string append;         // APPEND directive value
        std::string initrd;         // INITRD directive value
        int line = 0;
        bool sysappend = false;
        bool is_com32 = false;      // COM32 / COMBOOT / LOCALBOOT
        bool is_localboot = false;
        std::string com_path;       // path from COM32/COMBOOT
    };

    // globals
    int timeout_raw = -1;          // raw syslinux value (10ths of a second)
    std::string default_label;
    std::string menu_title;
    bool global_sysappend = false;

    // labels collected in order
    std::vector<SysLabel> labels;
    std::optional<SysLabel> current;

    auto flush_label = [&]() {
        if (current.has_value()) {
            labels.push_back(std::move(*current));
            current.reset();
        }
    };

    static const std::regex key_val_re(
        R"(^([A-Za-z0-9_]+)\s+(.*)$)");

    int lineno = 0;
    for (const std::string& raw : splitlines(text)) {
        ++lineno;
        std::string line = strip(raw);
        if (line.empty() || line[0] == '#') continue;

        // LABEL start
        if (starts_with(lower(line), "label ") ||
            lower(line) == "label") {
            flush_label();
            SysLabel lbl;
            lbl.line = lineno;
            if (line.size() > 5) {
                lbl.name = strip(line.substr(5));
            }
            current = std::move(lbl);
            continue;
        }

        std::smatch m;
        if (!std::regex_match(line, m, key_val_re)) {
            rep.note("line " + std::to_string(lineno) +
                     ": unrecognized syslinux directive ignored: '" + line + "'");
            continue;
        }
        std::string key = lower(m[1].str());
        std::string val = strip(m[2].str());

        if (current.has_value()) {
            // inside a LABEL block
            SysLabel& lbl = *current;
            if (key == "kernel") {
                lbl.kernel = val;
            } else if (key == "append") {
                if (!lbl.append.empty()) lbl.append += " ";
                lbl.append += val;
            } else if (key == "initrd") {
                lbl.initrd = val;
            } else if (key == "menu") {
                // MENU LABEL / MENU TITLE
                std::string rest = val;
                if (starts_with(lower(rest), "label ")) {
                    lbl.menu_label = strip(rest.substr(6));
                } else if (starts_with(lower(rest), "title ")) {
                    menu_title = strip(rest.substr(6));
                } else {
                    rep.note("line " + std::to_string(lineno) +
                             ": unknown MENU sub-directive ignored");
                }
            } else if (key == "sysappend") {
                lbl.sysappend = (val == "1" || lower(val) == "true" ||
                                 lower(val) == "yes");
            } else if (key == "com32" || key == "comboot") {
                lbl.is_com32 = true;
                lbl.com_path = val;
            } else if (key == "localboot") {
                lbl.is_localboot = true;
            } else {
                rep.note("line " + std::to_string(lineno) +
                         ": unknown LABEL directive '" + key + "' ignored");
            }
        } else {
            // global directive
            if (key == "timeout") {
                try {
                    timeout_raw = std::stoi(val);
                } catch (...) {
                    rep.note("line " + std::to_string(lineno) +
                             ": invalid TIMEOUT '" + val + "' ignored");
                }
            } else if (key == "default") {
                default_label = val;
            } else if (key == "menu") {
                std::string rest = val;
                if (starts_with(lower(rest), "title ")) {
                    menu_title = strip(rest.substr(6));
                } else {
                    rep.note("line " + std::to_string(lineno) +
                             ": unknown global MENU directive ignored");
                }
            } else if (key == "sysappend") {
                global_sysappend = (val == "1" || lower(val) == "true" ||
                                    lower(val) == "yes");
            } else if (key == "include") {
                // already handled in read_recursive
            } else {
                rep.note("line " + std::to_string(lineno) +
                         ": unknown global syslinux directive '" + key +
                         "' ignored");
            }
        }
    }
    flush_label();

    // ---- convert timeout: syslinux uses 10ths of a second -----------------
    if (timeout_raw >= 0) {
        cfg.timeout = static_cast<int>(std::lround(timeout_raw / 10.0));
    }

    // ---- default label ----------------------------------------------------
    if (!default_label.empty()) {
        cfg.def.kind = DefaultSpec::Path;
        cfg.def.str = default_label;
    }

    // ---- menu title (stored as note; ForeB doesn't use it directly) -------
    if (!menu_title.empty()) {
        rep.note("syslinux MENU TITLE '" + menu_title +
                 "' noted; ForeB uses its own menu title");
    }

    // ---- helper: strip leading "Boot " / "Install " from menu label -------
    auto clean_menu_label = [&](const std::string& ml) -> std::string {
        std::string s = ml;
        if (starts_with(lower(s), "boot "))
            s = strip(s.substr(5));
        else if (starts_with(lower(s), "install "))
            s = strip(s.substr(8));
        return s;
    };

    // ---- helper: extract initrd= from APPEND line -------------------------
    auto extract_initrd_from_append = [&](const std::string& append_line)
        -> std::pair<std::string, std::string> {
        // returns (cleaned_append, initrd_path_or_empty)
        std::string cleaned;
        std::string initrd;
        std::istringstream iss(append_line);
        std::string tok;
        while (iss >> tok) {
            if (starts_with(lower(tok), "initrd=")) {
                initrd = tok.substr(7);
            } else {
                if (!cleaned.empty()) cleaned += " ";
                cleaned += tok;
            }
        }
        return {cleaned, initrd};
    };

    // ---- translate each LABEL into an OutEntry ----------------------------
    for (const auto& lbl : labels) {
        std::string where = "line " + std::to_string(lbl.line) + ": ";

        // determine title
        std::string title;
        if (!lbl.menu_label.empty()) {
            title = clean_menu_label(lbl.menu_label);
        } else if (!lbl.name.empty()) {
            title = lbl.name;
        } else {
            rep.warn(where + "LABEL with no name or MENU LABEL; skipped");
            continue;
        }
        title = sanitize_title(title);

        // COM32 / COMBOOT / LOCALBOOT -> chainload
        if (lbl.is_com32 || lbl.is_localboot) {
            auto entry = std::make_shared<OutEntry>();
            entry->title = title;
            entry->type = "chainload";
            if (lbl.is_localboot) {
                entry->chain = "";
                rep.note(where + "LOCALBOOT entry; mapped to chainload "
                         "(BIOS-only, unlikely to work on UEFI)");
            } else {
                entry->chain = normalize_esp_path(lbl.com_path);
            }
            entry->icon = guess_icon(title + " " + entry->chain,
                                     entry->type);
            cfg.index_entries.push_back(entry);
            cfg.roots.push_back(make_entry(entry));
            continue;
        }

        // determine kernel path
        std::string kernel_path = lbl.kernel;
        if (kernel_path.empty()) {
            rep.warn(where + "entry '" + title +
                     "': no KERNEL directive; skipped");
            continue;
        }

        // separate initrd= from APPEND
        std::string cmdline, initrd_from_append;
        std::tie(cmdline, initrd_from_append) =
            extract_initrd_from_append(lbl.append);

        // if INITRD directive was given it overrides initrd= in APPEND
        std::string initrd_path = lbl.initrd.empty() ? initrd_from_append
                                                      : lbl.initrd;

        // determine type by kernel path
        std::string kl = lower(kernel_path);
        bool is_efi = ends_with(kl, ".efi") ||
                       kl.find("/boot/efi/") != std::string::npos ||
                       kl.find("\\efi\\") != std::string::npos;

        auto entry = std::make_shared<OutEntry>();
        entry->title = title;
        entry->cmdline = cmdline;
        entry->source_name = lbl.name;

        if (is_efi) {
            entry->type = "chainload";
            entry->chain = normalize_esp_path(kernel_path);
        } else {
            // assume linux kernel (vmlinuz, bzImage, etc.)
            entry->type = "linux";
            entry->vmlinuz = normalize_esp_path(kernel_path);
            if (!initrd_path.empty()) {
                entry->initrd = normalize_esp_path(initrd_path);
            }
        }

        // SYSAPPEND flag: note it, ForeB doesn't use it directly
        if (lbl.sysappend || global_sysappend) {
            rep.note(where + "SYSAPPEND flag set; ignored by ForeB "
                     "(ForeB handles append natively)");
        }

        std::string base = !entry->vmlinuz.empty() ? entry->vmlinuz
                                                    : entry->chain;
        entry->icon = guess_icon(title + " " + base_name(base),
                                 entry->type);
        cfg.index_entries.push_back(entry);
        cfg.roots.push_back(make_entry(entry));
    }

    // ---- apply default label -> DefaultSpec --------------------------------
    // We matched the label name earlier; now try to find its index
    if (!default_label.empty() && cfg.def.kind == DefaultSpec::Path) {
        for (size_t i = 0; i < labels.size(); ++i) {
            if (labels[i].name == default_label) {
                cfg.def.kind = DefaultSpec::Index;
                cfg.def.index = static_cast<int>(i) + 1;  // 1-based
                cfg.def.str.clear();
                break;
            }
        }
        // if not found by name, keep as Path (pattern match)
    }

    return cfg;
}

// ===========================================================================
//  rEFInd parser
// ===========================================================================
struct RefindEntry {
    std::string title;
    int line = 0;
    std::string loader;
    std::string icon;
    std::string options;
    std::string ostype;
    std::string initrd;
    bool disabled = false;
    std::vector<std::shared_ptr<RefindEntry>> submenuentries;
};

static std::string refind_resolve_path(const std::string& p, EspContext& /*ctx*/,
                                       Reporter& /*rep*/) {
    std::string v = strip(p);
    if (v.empty()) return v;
    // rEFInd paths are ESP-relative, e.g. \EFI\refind\grubx64.efi
    // Convert backslash to forward slash
    v = replace_all(v, "\\", "/");
    return normalize_esp_path(v);
}

ParsedConfig parse_refind(const std::string& path, EspContext& ctx,
                          Reporter& rep) {
    std::string text = read_text(path);
    ParsedConfig cfg;
    cfg.kind = "refind";
    cfg.source_path = path;
    rep.note("rEFInd parsing is best-effort");

    std::map<std::string, std::string> globals_;
    std::vector<std::string> global_order;

    // Regex patterns
    static const std::regex me_re(
        R"(^\s*(menuentry|ENTRY)\s+["']?([^"'\{]+)["']?\s*(\{)?)",
        std::regex::icase);
    static const std::regex me_no_brace_re(
        R"(^\s*(menuentry|ENTRY)\s+["']?([^"'\{]+)["']?)",
        std::regex::icase);
    static const std::regex sub_re(
        R"(^\s*submenuentry\s+["']?([^"'\{]+)["']?\s*(\{)?)",
        std::regex::icase);
    static const std::regex kv_re(
        R"(^\s*(loader|icon|options|ostype|initrd|disabled)\s+(.*))",
        std::regex::icase);
    static const std::regex global_kv_re(
        R"(^\s*(timeout|default_selection|scanfor|dont_scan_dirs|"
        R"dont_scan_volumes|hideui|ui_banner|ui_smallbanner|"
        R"also_scan_dirs)\s+(.*))",
        std::regex::icase);

    // Simple state machine for braced blocks
    enum class Scope { Global, Entry, Submenu };

    struct Frame {
        Scope scope;
        std::string title;
        int line;
        std::string loader;
        std::string icon;
        std::string options;
        std::string ostype;
        std::string initrd;
        bool disabled = false;
    };

    std::vector<Frame> stack;
    std::vector<std::shared_ptr<RefindEntry>> top_entries;

    auto current_frame = [&]() -> Frame* {
        return stack.empty() ? nullptr : &stack.back();
    };

    auto flush_entry = [&](Frame& f) {
        auto e = std::make_shared<RefindEntry>();
        e->title = f.title;
        e->line = f.line;
        e->loader = f.loader;
        e->icon = f.icon;
        e->options = f.options;
        e->ostype = f.ostype;
        e->initrd = f.initrd;
        e->disabled = f.disabled;
        if (f.scope == Scope::Submenu && !stack.empty()) {
            // find parent entry frame
            for (int i = static_cast<int>(stack.size()) - 2; i >= 0; --i) {
                if (stack[i].scope == Scope::Entry) {
                    // attach as submenuentry to parent entry somehow
                    // We'll flatten submenus later
                    break;
                }
            }
        }
        // For simplicity, flat top-level entries
        top_entries.push_back(std::move(e));
    };

    int lineno = 0;
    for (const std::string& raw : splitlines(text)) {
        ++lineno;
        std::string line = strip(raw);
        if (line.empty() || line[0] == '#') continue;

        std::smatch m;

        // Count braces to track nesting
        auto count_ch = [](const std::string& s, char c) {
            int n = 0;
            for (char ch : s)
                if (ch == c) ++n;
            return n;
        };

        // If inside a block, check for directives
        Frame* cur = current_frame();
        if (cur) {
            // Check for closing brace
            if (line[0] == '}') {
                flush_entry(*cur);
                stack.pop_back();
                int extra_closes = count_ch(line, '}') - 1;
                while (extra_closes > 0 && !stack.empty()) {
                    flush_entry(stack.back());
                    stack.pop_back();
                    --extra_closes;
                }
                continue;
            }

            // Parse directives inside a block
            if (std::regex_match(line, m, kv_re)) {
                std::string key = lower(m[1].str());
                std::string val = strip(m[2].str());
                if (key == "loader") cur->loader = refind_resolve_path(val, ctx, rep);
                else if (key == "icon") cur->icon = refind_resolve_path(val, ctx, rep);
                else if (key == "options") cur->options = strip(val);
                else if (key == "ostype") cur->ostype = strip(val);
                else if (key == "initrd") cur->initrd = refind_resolve_path(val, ctx, rep);
                else if (key == "disabled") cur->disabled = true;
                continue;
            }

            // Check for submenuentry inside a block
            std::smatch sub_m;
            if (std::regex_match(line, sub_m, sub_re)) {
                Frame sub;
                sub.scope = Scope::Submenu;
                sub.title = strip(sub_m[1].str());
                sub.line = lineno;
                // Check for opening brace
                std::string after = line.substr(sub_m.position(0) + sub_m.length(0));
                if (count_ch(after, '{') > 0 || sub_m[2].matched) {
                    stack.push_back(sub);
                } else {
                    // no brace: this submenuentry owns directives until next
                    // submenuentry, menuentry, or closing brace
                    stack.push_back(sub);
                }
                continue;
            }

            // If inside Submenu scope and line doesn't match any known pattern,
            // fall through to check for opening braces of new blocks below.
        }

        // Check for menuentry / ENTRY
        std::smatch me_m;
        if (std::regex_match(line, me_m, me_re)) {
            Frame f;
            f.scope = Scope::Entry;
            f.title = strip(me_m[2].str());
            f.line = lineno;
            std::string after = line.substr(me_m.position(0) + me_m.length(0));
            bool has_brace = count_ch(after, '{') > 0 || me_m[3].matched;
            if (has_brace) {
                stack.push_back(f);
            } else {
                // block-less menuentry: directives until next menuentry/EOF
                stack.push_back(f);
            }
            continue;
        }

        // Check for menuentry without braces (no regex match due to quotes)
        std::smatch me_nb;
        if (std::regex_match(line, me_nb, me_no_brace_re)) {
            Frame f;
            f.scope = Scope::Entry;
            f.title = strip(me_nb[2].str());
            f.line = lineno;
            stack.push_back(f);
            continue;
        }

        // Global directives
        if (std::regex_match(line, m, global_kv_re)) {
            std::string key = lower(m[1].str());
            std::string val = strip(m[2].str());
            if (!globals_.count(key)) global_order.push_back(key);
            globals_[key] = val;
            continue;
        }

        // Ignore unrecognized lines
        rep.note("line " + std::to_string(lineno) + ": ignored: '" + line + "'");
    }

    // Close any remaining open frames
    while (!stack.empty()) {
        rep.note("line " + std::to_string(stack.back().line) +
                 ": unbalanced block; closed at EOF");
        flush_entry(stack.back());
        stack.pop_back();
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
        } else if (key == "default_selection") {
            bool digits = !value.empty() &&
                std::all_of(value.begin(), value.end(),
                            [](char c) { return std::isdigit((unsigned char)c); });
            if (digits) {
                cfg.def.kind = DefaultSpec::Index;
                cfg.def.index = std::stoi(value) + 1;  // rEFInd is 0-based
            } else if (!value.empty()) {
                cfg.def.kind = DefaultSpec::Pattern;
                cfg.def.str = sanitize_title(value);
            }
        } else if (key == "scanfor" || key == "dont_scan_dirs" ||
                   key == "dont_scan_volumes" || key == "hideui" ||
                   key == "ui_banner" || key == "ui_smallbanner" ||
                   key == "also_scan_dirs") {
            rep.note("global '" + key + "' ignored (cosmetic/unsupported)");
        } else {
            rep.note("unknown rEFInd global '" + key + "' ignored");
        }
    }

    // ---- translate entries --------------------------------------------------
    for (const auto& e : top_entries) {
        std::string where = "line " + std::to_string(e->line) + ": ";

        if (e->disabled) {
            rep.note(where + "entry '" + e->title +
                     "' is disabled; skipped");
            continue;
        }

        if (e->loader.empty()) {
            rep.note(where + "entry '" + e->title +
                     "' has no loader; skipped");
            continue;
        }

        std::string title = sanitize_title(e->title);
        std::string loader_lower = lower(e->loader);

        std::shared_ptr<OutEntry> out = std::make_shared<OutEntry>();
        out->title = title;

        if (loader_lower.size() >= 4 &&
            loader_lower.substr(loader_lower.size() - 4) == ".efi") {
            out->type = "chainload";
            out->chain = e->loader;
        } else if (loader_lower.find("vmlinuz") != std::string::npos ||
                   loader_lower.find("bzimage") != std::string::npos ||
                   loader_lower.find("vmlinux") != std::string::npos) {
            out->type = "linux";
            out->vmlinuz = e->loader;
            out->initrd = e->initrd;
            out->cmdline = e->options;
        } else if (loader_lower.find("multiboot") != std::string::npos) {
            out->type = "forest";
            out->kernel = e->loader;
            out->cmdline = e->options;
        } else {
            // default to chainload for unknown loaders
            out->type = "chainload";
            out->chain = e->loader;
            rep.note(where + "entry '" + e->title +
                     "': unknown loader type; treating as chainload");
        }

        out->icon = e->icon.empty()
                        ? guess_icon(e->title + " " + base_name(e->loader),
                                     out->type)
                        : e->icon;

        cfg.index_entries.push_back(out);
        cfg.roots.push_back(make_entry(out));
    }

    return cfg;
}

// ===========================================================================
//  ZFS Boot Menu (ZBM) parser
// ===========================================================================

// Parse a single shell-style KEY=VALUE file.
static std::map<std::string, std::string> parse_shell_kv(
        const std::string& text, Reporter& rep, const std::string& where) {
    std::map<std::string, std::string> kv;
    static const std::regex kv_re(
        R"(^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*?)\s*$)");
    int lineno = 0;
    for (const std::string& raw : splitlines(text)) {
        ++lineno;
        std::string line = strip(raw);
        if (line.empty() || line[0] == '#') continue;
        // handle export prefix
        if (starts_with(line, "export ")) line = strip(line.substr(7));
        std::smatch m;
        if (!std::regex_match(line, m, kv_re)) {
            rep.note(where + "line " + std::to_string(lineno) +
                     ": not a KEY=VALUE pair, ignored: '" + line + "'");
            continue;
        }
        std::string key = m[1].str();
        std::string val = m[2].str();
        // strip surrounding quotes
        if (val.size() >= 2 &&
            ((val.front() == '"' && val.back() == '"') ||
             (val.front() == '\'' && val.back() == '\'')))
            val = val.substr(1, val.size() - 2);
        kv[key] = val;
    }
    return kv;
}

// Try to list ZFS boot environments (filesystem datasets).
static std::vector<std::string> list_zfs_boot_envs(Reporter& rep) {
    auto out = run_cmd({"zfs", "list", "-t", "filesystem", "-H", "-o", "name"});
    if (!out.has_value()) {
        rep.note("zfs not available; cannot enumerate boot environments");
        return {};
    }
    std::vector<std::string> bes;
    for (const std::string& line : splitlines(*out)) {
        std::string s = strip(line);
        if (!s.empty()) bes.push_back(s);
    }
    return bes;
}

ParsedConfig parse_zfsbootmenu(const std::string& path, EspContext& ctx,
                               Reporter& rep) {
    (void)ctx;
    ParsedConfig cfg;
    cfg.kind = "zfsbootmenu";
    cfg.source_path = path;

    // ---- collect all config sources ----------------------------------------
    // Primary: the path argument (typically /etc/default/zfsbootmenu)
    // Snippets: /etc/zfsbootmenu.d/*.conf
    std::map<std::string, std::string> merged;  // key -> value (last wins)
    std::vector<std::string> source_files;

    auto load_file = [&](const std::string& p) {
        std::error_code ec;
        if (!fs::is_regular_file(p, ec)) return;
        std::string text;
        try {
            text = read_text(p);
        } catch (...) {
            return;
        }
        source_files.push_back(p);
        auto kv = parse_shell_kv(text, rep, p + ": ");
        for (auto& [k, v] : kv)
            merged[k] = std::move(v);
    };

    load_file(path);

    // load snippets directory if it exists
    static const std::string snippets_dir = "/etc/zfsbootmenu.d";
    {
        std::error_code ec;
        if (fs::is_directory(snippets_dir, ec)) {
            std::vector<std::string> names;
            for (const auto& e : fs::directory_iterator(snippets_dir, ec)) {
                std::string n = e.path().filename().string();
                if (ends_with(n, ".conf")) names.push_back(n);
            }
            std::sort(names.begin(), names.end());
            for (const auto& n : names)
                load_file(snippets_dir + "/" + n);
        }
    }

    if (merged.empty()) {
        rep.note("no ZBM config values found in " + path);
    }

    // ---- extract global settings -------------------------------------------
    auto get_zbm = [&](const std::string& k) -> std::string {
        auto it = merged.find(k);
        return it == merged.end() ? std::string() : it->second;
    };

    // ZBM_TIMEOUT
    {
        std::string t = get_zbm("ZBM_TIMEOUT");
        if (!t.empty()) {
            try {
                cfg.timeout = static_cast<int>(std::lround(std::stod(t)));
            } catch (...) {
                rep.note("invalid ZBM_TIMEOUT '" + t + "' ignored");
            }
        }
    }

    // ZBM_DEFAULT -> DefaultSpec
    {
        std::string def = get_zbm("ZBM_DEFAULT");
        if (!def.empty()) {
            cfg.def.kind = DefaultSpec::Pattern;
            cfg.def.str = def;
        }
    }

    // Report ignored/unsupported settings
    static const std::set<std::string> known_keys = {
        "ZBM_TIMEOUT", "ZBM_DEFAULT", "ZBM_KERNEL", "ZBM_INITRD",
        "ZBM_CMDLINE", "ZBM_HOSTID", "ZBM_POOL", "ZBM_GENERATE",
        "ZBM_COMMENTS", "ZBM_SORT", "ZBM_SUBMENU",
    };
    for (const auto& [k, v] : merged) {
        if (k == "ZBM_TIMEOUT" || k == "ZBM_DEFAULT") continue;
        if (k == "ZBM_GENERATE") {
            std::string lv = lower(v);
            if (lv == "yes" || lv == "true" || lv == "1" || lv == "on") {
                rep.note("ZBM_GENERATE enabled; entries will be auto-generated");
            }
            continue;
        }
        if (k == "ZBM_POOL" || k == "ZBM_HOSTID" || k == "ZBM_KERNEL" ||
            k == "ZBM_INITRD" || k == "ZBM_CMDLINE") {
            rep.note("ZBM setting '" + k + "' noted (used for entry generation)");
            continue;
        }
        if (!starts_with(k, "ZBM_")) {
            // non-ZBM key, ignore silently
            continue;
        }
        rep.note("unknown ZBM setting '" + k + "' ignored");
    }

    // ---- generate boot entries from ZFS datasets ---------------------------
    std::string zbm_kernel = get_zbm("ZBM_KERNEL");
    std::string zbm_initrd = get_zbm("ZBM_INITRD");
    std::string zbm_cmdline = get_zbm("ZBM_CMDLINE");
    std::string zbm_pool = get_zbm("ZBM_POOL");

    auto bes = list_zfs_boot_envs(rep);
    if (!bes.empty()) {
        // If pool is specified, filter to that pool's datasets
        if (!zbm_pool.empty()) {
            std::vector<std::string> filtered;
            for (const auto& ds : bes) {
                if (starts_with(ds, zbm_pool + "/") || ds == zbm_pool)
                    filtered.push_back(ds);
            }
            if (!filtered.empty())
                bes = std::move(filtered);
            else
                rep.note("ZBM_POOL '" + zbm_pool +
                         "' matched no datasets; using all");
        }

        for (const auto& ds : bes) {
            auto entry = std::make_shared<OutEntry>();
            // derive title from dataset: strip pool prefix, use last component
            std::string title;
            {
                std::string rest = ds;
                size_t slash = rest.find('/');
                if (slash != std::string::npos)
                    rest = rest.substr(slash + 1);
                // replace remaining slashes with '>'
                size_t pos;
                while ((pos = rest.find('/')) != std::string::npos)
                    rest.replace(pos, 1, " > ");
                title = rest;
            }
            entry->title = sanitize_title(title);
            entry->type = "linux";
            entry->source_name = ds;

            // kernel path within the BE
            if (!zbm_kernel.empty()) {
                entry->vmlinuz = "/" + ds + "/" + zbm_kernel;
            } else {
                // common default paths
                entry->vmlinuz = "/" + ds + "/boot/vmlinuz";
            }
            // initrd path within the BE
            if (!zbm_initrd.empty()) {
                entry->initrd = "/" + ds + "/" + zbm_initrd;
            } else {
                entry->initrd = "/" + ds + "/boot/initramfs.img";
            }

            entry->cmdline = zbm_cmdline;
            entry->icon = guess_icon(entry->title + " zfs", entry->type);
            cfg.index_entries.push_back(entry);
            cfg.roots.push_back(make_entry(entry));
        }
    } else {
        // Fallback: generate a single placeholder entry
        rep.note("generating placeholder ZBM entry (no ZFS available)");
        auto entry = std::make_shared<OutEntry>();
        entry->title = "ZFS Boot Menu";
        entry->type = "linux";
        entry->vmlinuz = "/BOOT/vmlinuz";
        entry->initrd = "/BOOT/initramfs.img";
        entry->cmdline = zbm_cmdline;
        entry->icon = guess_icon("zfs boot menu", entry->type);
        entry->source_name = "zfsbootmenu";
        cfg.index_entries.push_back(entry);
        cfg.roots.push_back(make_entry(entry));
    }

    return cfg;
}

// ===========================================================================
//  Clover parser (config.plist or clover.conf)
// ===========================================================================

static std::string clover_normalize_path(const std::string& raw) {
    std::string p = raw;
    // Clover uses backslashes in paths; normalize to forward slashes.
    std::replace(p.begin(), p.end(), '\\', '/');
    // Strip leading slash if present (paths are ESP-relative).
    if (!p.empty() && p.front() == '/') p = p.substr(1);
    return normalize_esp_path(p);
}

static std::string clover_extract_text(const std::string& xml,
                                       size_t& pos) {
    // Consume text content until the next '<'.
    size_t end = xml.find('<', pos);
    if (end == std::string::npos) end = xml.size();
    std::string text = xml.substr(pos, end - pos);
    pos = end;
    return text;
}

static std::string clover_extract_tag_text(const std::string& xml,
                                           const std::string& tag,
                                           size_t& pos) {
    // Find <tag>...</tag> and return inner text.
    std::string open = "<" + tag + ">";
    std::string close = "</" + tag + ">";
    size_t o = xml.find(open, pos);
    if (o == std::string::npos) return "";
    size_t start = o + open.size();
    size_t c = xml.find(close, start);
    if (c == std::string::npos) return "";
    std::string text = xml.substr(start, c - pos);
    pos = c + close.size();
    return text;
}

static std::string clover_extract_tag_value(const std::string& xml,
                                            const std::string& tag) {
    size_t pos = 0;
    return clover_extract_tag_text(xml, tag, pos);
}

static std::vector<std::string> clover_split_plist(const std::string& xml) {
    // Split XML into top-level plist value tokens: <dict>, <array>, <string>,
    // <integer>, <true/>, <false/>. We only need a flat scan for GUI/Entries.
    std::vector<std::string> tokens;
    size_t i = 0;
    while (i < xml.size()) {
        size_t lt = xml.find('<', i);
        if (lt == std::string::npos) break;
        // Skip XML comments.
        if (xml.compare(lt, 4, "<!--") == 0) {
            size_t ce = xml.find("-->", lt + 4);
            i = (ce == std::string::npos) ? xml.size() : ce + 3;
            continue;
        }
        // Skip <?xml ... ?> processing instructions.
        if (xml.compare(lt, 2, "<?") == 0) {
            size_t ce = xml.find("?>", lt + 2);
            i = (ce == std::string::npos) ? xml.size() : ce + 2;
            continue;
        }
        // Skip <plist ...> wrapper.
        if (xml.compare(lt, 6, "<plist") == 0) {
            size_t ce = xml.find('>', lt + 6);
            i = (ce == std::string::npos) ? xml.size() : ce + 1;
            continue;
        }
        // Self-closing tags: <true/>, <false/>, <empty/>.
        if (xml.compare(lt, 7, "<true/>") == 0) {
            tokens.push_back("true");
            i = lt + 7;
            continue;
        }
        if (xml.compare(lt, 8, "<false/>") == 0) {
            tokens.push_back("false");
            i = lt + 8;
            continue;
        }
        if (xml.compare(lt, 8, "<empty/>") == 0) {
            i = lt + 8;
            continue;
        }
        // Opening/closing tags.
        size_t gt = xml.find('>', lt + 1);
        if (gt == std::string::npos) break;
        std::string tag_str = xml.substr(lt, gt - lt + 1);
        if (tag_str.back() == '/' && tag_str.size() > 2) {
            // Self-closing custom tag (e.g. <data/>).
            tokens.push_back(tag_str);
        } else if (tag_str[1] == '/') {
            // Closing tag.
            tokens.push_back(tag_str);
        } else {
            // Opening tag.
            tokens.push_back(tag_str);
        }
        i = gt + 1;
    }
    return tokens;
}

static std::optional<std::map<std::string, std::string>>
parse_plist_dict(const std::string& xml, size_t& pos) {
    // Parse a <dict> ... </dict> block into a flat key->value map.
    // Returns nullopt if the dict is malformed.
    std::map<std::string, std::string> result;
    // Skip to opening <dict>
    size_t d = xml.find("<dict>", pos);
    if (d == std::string::npos) return std::nullopt;
    pos = d + 6;
    int depth = 1;
    std::string current_key;
    while (pos < xml.size() && depth > 0) {
        size_t lt = xml.find('<', pos);
        if (lt == std::string::npos) break;
        // Extract any text between pos and lt as potential content.
        // For self-closing <true/> <false/> inside a dict value, handle below.
        if (xml.compare(lt, 7, "<true/>") == 0) {
            if (!current_key.empty()) {
                result[current_key] = "true";
                current_key.clear();
            }
            pos = lt + 7;
            continue;
        }
        if (xml.compare(lt, 8, "<false/>") == 0) {
            if (!current_key.empty()) {
                result[current_key] = "false";
                current_key.clear();
            }
            pos = lt + 8;
            continue;
        }
        if (xml.compare(lt, 8, "<empty/>") == 0) {
            pos = lt + 8;
            continue;
        }
        size_t gt = xml.find('>', lt + 1);
        if (gt == std::string::npos) break;
        std::string tag = xml.substr(lt + 1, gt - lt - 1);
        // Remove any attributes (shouldn't be there in plist, but be safe).
        size_t sp = tag.find(' ');
        if (sp != std::string::npos) tag = tag.substr(0, sp);
        bool self_close = (xml[gt - 1] == '/');
        if (tag == "dict") {
            if (!self_close) ++depth;
            pos = gt + 1;
            continue;
        }
        if (tag[0] == '/' && tag.size() > 1) {
            std::string bare = tag.substr(1);
            if (bare == "dict") {
                --depth;
                pos = gt + 1;
                continue;
            }
            // Closing any other tag (e.g. </string>, </integer>).
            pos = gt + 1;
            continue;
        }
        // Opening tag for a value type.
        if (tag == "key") {
            size_t close = xml.find("</key>", gt + 1);
            if (close == std::string::npos) break;
            current_key = xml.substr(gt + 1, close - gt - 1);
            pos = close + 6;
            continue;
        }
        // For string, integer, data, real, date - extract text content.
        std::string value;
        if (!self_close) {
            size_t close_tag = xml.find("</" + tag + ">", gt + 1);
            if (close_tag != std::string::npos) {
                value = xml.substr(gt + 1, close_tag - gt - 1);
                pos = close_tag + tag.size() + 3;
            } else {
                value = xml.substr(gt + 1);
                pos = xml.size();
            }
        } else {
            pos = gt + 1;
        }
        if (!current_key.empty()) {
            // Trim whitespace from value.
            size_t start = value.find_first_not_of(" \t\n\r");
            size_t end = value.find_last_not_of(" \t\n\r");
            if (start != std::string::npos)
                result[current_key] = value.substr(start, end - start + 1);
            current_key.clear();
        }
    }
    return result;
}

static std::vector<std::map<std::string, std::string>>
parse_plist_array_of_dicts(const std::string& xml,
                           const std::string& array_tag_path,
                           size_t start_pos) {
    // Find an <array> at array_tag_path (e.g. "GUI/Entries") and parse its
    // children as <dict> elements. Since we don't have a proper tree, we scan
    // for the nested structure manually.
    //
    // Strategy: find the <array> that follows the last key in the path, then
    // collect <dict> blocks until </array>.
    std::vector<std::map<std::string, std::string>> results;
    // Split path into segments.
    std::vector<std::string> segs;
    {
        std::istringstream iss(array_tag_path);
        std::string seg;
        while (std::getline(iss, seg, '/')) {
            if (!seg.empty()) segs.push_back(seg);
        }
    }
    // Walk the plist to find the <array>.
    size_t pos = start_pos;
    for (size_t i = 0; i < segs.size(); ++i) {
        // Find <key>seg</key> followed by <dict> or <array>.
        std::string key_pattern = "<key>" + segs[i] + "</key>";
        size_t kpos = xml.find(key_pattern, pos);
        if (kpos == std::string::npos) return results;
        pos = kpos + key_pattern.size();
    }
    // Now find the next <array>.
    size_t arr = xml.find("<array>", pos);
    if (arr == std::string::npos) return results;
    pos = arr + 7;
    // Find matching </array>.
    int depth = 1;
    size_t arr_end = pos;
    while (arr_end < xml.size() && depth > 0) {
        size_t ot = xml.find("<array>", arr_end);
        size_t ct = xml.find("</array>", arr_end);
        if (ct == std::string::npos) break;
        if (ot != std::string::npos && ot < ct) {
            ++depth;
            arr_end = ot + 7;
        } else {
            --depth;
            if (depth == 0) {
                arr_end = ct;
            } else {
                arr_end = ct + 8;
            }
        }
    }
    // Extract the array content.
    std::string arr_content = xml.substr(pos, arr_end - pos);
    // Parse each <dict> in the array.
    size_t dp = 0;
    while (dp < arr_content.size()) {
        size_t d = arr_content.find("<dict>", dp);
        if (d == std::string::npos) break;
        // Find matching </dict>.
        int dd = 1;
        size_t dep = d + 6;
        while (dep < arr_content.size() && dd > 0) {
            size_t ot = arr_content.find("<dict>", dep);
            size_t ct = arr_content.find("</dict>", dep);
            if (ct == std::string::npos) break;
            if (ot != std::string::npos && ot < ct) {
                ++dd;
                dep = ot + 6;
            } else {
                --dd;
                if (dd == 0) {
                    dep = ct + 7;
                } else {
                    dep = ct + 7;
                }
            }
        }
        std::string dict_xml = arr_content.substr(d, dep - d);
        size_t dummy = 0;
        auto dict = parse_plist_dict(dict_xml, dummy);
        if (dict.has_value()) results.push_back(*dict);
        dp = dep;
    }
    return results;
}

ParsedConfig parse_clover(const std::string& path, EspContext& ctx,
                          Reporter& rep) {
    (void)ctx;
    ParsedConfig cfg;
    cfg.source_path = path;
    std::string text = read_text(path);

    // Detect format by file extension.
    std::string ext = lower(fs::path(path).extension().string());
    bool is_plist = (ext == ".plist");

    if (is_plist) {
        cfg.kind = "clover";
        // ---- Parse GUI/Timeout ----
        // Find <key>GUI</key><dict> ... then find <key>Timeout</key><integer>N</integer>
        {
            size_t gui_key = text.find("<key>GUI</key>");
            if (gui_key != std::string::npos) {
                size_t d = text.find("<dict>", gui_key + 14);
                if (d != std::string::npos) {
                    size_t timeout_key = text.find("<key>Timeout</key>", d);
                    if (timeout_key != std::string::npos) {
                        size_t ival = text.find("<integer>", timeout_key);
                        if (ival != std::string::npos) {
                            size_t close = text.find("</integer>", ival);
                            if (close != std::string::npos) {
                                std::string val =
                                    text.substr(ival + 9, close - ival - 9);
                                try {
                                    cfg.timeout = static_cast<int>(
                                        std::lround(std::stod(strip(val))));
                                } catch (...) {
                                    rep.note("clover: invalid Timeout '" +
                                             val + "' ignored");
                                }
                            }
                        }
                    }
                }
            }
        }

        // ---- Parse GUI/DefaultVolume ----
        {
            size_t gui_key = text.find("<key>GUI</key>");
            if (gui_key != std::string::npos) {
                size_t d = text.find("<dict>", gui_key + 14);
                if (d != std::string::npos) {
                    size_t dv_key =
                        text.find("<key>DefaultVolume</key>", d);
                    if (dv_key != std::string::npos) {
                        size_t s = text.find("<string>", dv_key);
                        if (s != std::string::npos) {
                            size_t e = text.find("</string>", s);
                            if (e != std::string::npos) {
                                std::string vol =
                                    strip(text.substr(s + 8, e - s - 8));
                                if (!vol.empty()) {
                                    cfg.def.kind = DefaultSpec::Pattern;
                                    cfg.def.str = vol;
                                }
                            }
                        }
                    }
                }
            }
        }

        // ---- Parse GUI/DefaultEntry ----
        {
            size_t gui_key = text.find("<key>GUI</key>");
            if (gui_key != std::string::npos) {
                size_t d = text.find("<dict>", gui_key + 14);
                if (d != std::string::npos) {
                    size_t de_key =
                        text.find("<key>DefaultEntry</key>", d);
                    if (de_key != std::string::npos) {
                        size_t dict_start = text.find("<dict>", de_key);
                        if (dict_start != std::string::npos) {
                            size_t dp = dict_start;
                            auto entry_def = parse_plist_dict(text, dp);
                            if (entry_def.has_value()) {
                                std::string vol =
                                    get(*entry_def, "Volume");
                                std::string ep = get(*entry_def, "Path");
                                if (!vol.empty() && !ep.empty()) {
                                    cfg.def.kind = DefaultSpec::Path;
                                    cfg.def.str = clover_normalize_path(ep);
                                }
                            }
                        }
                    }
                }
            }
        }

        // ---- Parse GUI/Entries ----
        auto entries =
            parse_plist_array_of_dicts(text, "GUI/Entries", 0);
        for (const auto& e : entries) {
            std::string title = get(e, "Title");
            std::string raw_path = get(e, "Path");
            std::string volume = get(e, "Volume");
            std::string kernel = get(e, "Kernel");
            std::string kflags = get(e, "KernelFlags");
            int clover_type = 0;
            {
                std::string ts = get(e, "Type");
                if (!ts.empty()) {
                    try { clover_type = std::stoi(ts); } catch (...) {}
                }
            }

            if (title.empty() && raw_path.empty() && kernel.empty()) {
                rep.note("clover: skipping empty GUI/Entries dict");
                continue;
            }
            if (title.empty()) {
                title = !raw_path.empty() ? base_name(raw_path)
                        : !kernel.empty() ? base_name(kernel) : "Unknown";
            }

            std::string path_lower = lower(raw_path);
            bool is_efi =
                ends_with(path_lower, ".efi") ||
                path_lower.find("bootx64.efi") != std::string::npos ||
                path_lower.find("grub") != std::string::npos;

            std::shared_ptr<OutEntry> entry;
            // Type mapping:
            //   0 = Linux, 1 = macOS, 2 = Windows, 3 = Other
            // Path containing bootx64.efi or grub -> chainload
            if (clover_type == 0 && !kernel.empty()) {
                // Linux entry with kernel field.
                entry = std::make_shared<OutEntry>();
                entry->title = sanitize_title(title);
                entry->type = "linux";
                entry->vmlinuz = clover_normalize_path(kernel);
                entry->cmdline = kflags;
            } else if (clover_type == 0 && is_efi) {
                // Linux type but no kernel, just an EFI path -> chainload.
                entry = std::make_shared<OutEntry>();
                entry->title = sanitize_title(title);
                entry->type = "chainload";
                entry->chain = clover_normalize_path(raw_path);
                entry->cmdline = kflags;
            } else if (clover_type == 2) {
                // Windows -> chainload.
                entry = std::make_shared<OutEntry>();
                entry->title = sanitize_title(title);
                entry->type = "chainload";
                entry->chain = clover_normalize_path(raw_path);
                entry->cmdline = kflags;
            } else if (clover_type == 3 && is_efi) {
                // Other with EFI path -> chainload.
                entry = std::make_shared<OutEntry>();
                entry->title = sanitize_title(title);
                entry->type = "chainload";
                entry->chain = clover_normalize_path(raw_path);
                entry->cmdline = kflags;
            } else if (is_efi) {
                // Default: if it's an EFI binary, chainload it.
                entry = std::make_shared<OutEntry>();
                entry->title = sanitize_title(title);
                entry->type = "chainload";
                entry->chain = clover_normalize_path(raw_path);
                entry->cmdline = kflags;
            } else if (!kernel.empty()) {
                // Has kernel but no specific type match -> treat as Linux.
                entry = std::make_shared<OutEntry>();
                entry->title = sanitize_title(title);
                entry->type = "linux";
                entry->vmlinuz = clover_normalize_path(kernel);
                entry->cmdline = kflags;
            } else {
                rep.warn("clover: entry '" + title +
                         "' has no bootable path; skipped");
                continue;
            }

            if (!volume.empty()) {
                entry->extra_comments.push_back("clover-volume=" + volume);
            }

            std::string base = !entry->vmlinuz.empty() ? entry->vmlinuz
                             : !entry->kernel.empty()  ? entry->kernel
                                                       : entry->chain;
            entry->icon =
                guess_icon(title + " " + base_name(base), entry->type);
            cfg.index_entries.push_back(entry);
            cfg.roots.push_back(make_entry(entry));
        }
    } else {
        // ---- clover.conf (INI-like) format ----
        cfg.kind = "clover";
        std::string current_section;
        // For [System] entries, accumulate key-value pairs per entry.
        // Each new "Title" key starts a new entry.
        std::map<std::string, std::string> current_entry;
        std::vector<std::map<std::string, std::string>> sys_entries;

        auto flush_sys_entry = [&]() {
            if (current_entry.empty()) return;
            // Only add if it has at least a Title or Path.
            if (get(current_entry, "Title").empty() &&
                get(current_entry, "Path").empty() &&
                get(current_entry, "Kernel").empty()) {
                current_entry.clear();
                return;
            }
            sys_entries.push_back(current_entry);
            current_entry.clear();
        };

        for (const std::string& raw : splitlines(text)) {
            std::string line = strip(raw);
            if (line.empty() || line[0] == '#') continue;

            // Section header.
            if (line.front() == '[' && line.back() == ']') {
                flush_sys_entry();
                current_section = lower(line.substr(1, line.size() - 2));
                continue;
            }

            // Key = Value.
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = strip(line.substr(0, eq));
            std::string value = strip(line.substr(eq + 1));

            if (current_section == "boot") {
                std::string lk = lower(key);
                if (lk == "timeout") {
                    try {
                        cfg.timeout = static_cast<int>(
                            std::lround(std::stod(value)));
                    } catch (...) {
                        rep.note("clover.conf: invalid Timeout '" +
                                 value + "' ignored");
                    }
                }
            } else if (current_section == "system") {
                std::string lk = lower(key);
                // "Title" signals a new entry boundary.
                if (lk == "title") {
                    flush_sys_entry();
                }
                current_entry[lk] = value;
            }
        }
        flush_sys_entry();

        for (const auto& e : sys_entries) {
            std::string title = get(e, "title");
            std::string raw_path = get(e, "path");
            std::string kernel = get(e, "kernel");
            std::string kflags = get(e, "kernelflags") +
                                 get(e, "kernel_flags");
            std::string volume = get(e, "volume");
            int clover_type = 0;
            {
                std::string ts = get(e, "type");
                if (!ts.empty()) {
                    try { clover_type = std::stoi(ts); } catch (...) {}
                }
            }

            if (title.empty() && raw_path.empty() && kernel.empty()) {
                rep.note("clover.conf: skipping empty [System] entry");
                continue;
            }
            if (title.empty()) {
                title = !raw_path.empty() ? base_name(raw_path)
                        : !kernel.empty() ? base_name(kernel) : "Unknown";
            }

            std::string path_lower = lower(raw_path);
            bool is_efi =
                ends_with(path_lower, ".efi") ||
                path_lower.find("bootx64.efi") != std::string::npos ||
                path_lower.find("grub") != std::string::npos;

            std::shared_ptr<OutEntry> entry;
            if (clover_type == 0 && !kernel.empty()) {
                entry = std::make_shared<OutEntry>();
                entry->title = sanitize_title(title);
                entry->type = "linux";
                entry->vmlinuz = clover_normalize_path(kernel);
                entry->cmdline = kflags;
            } else if (clover_type == 0 && is_efi) {
                entry = std::make_shared<OutEntry>();
                entry->title = sanitize_title(title);
                entry->type = "chainload";
                entry->chain = clover_normalize_path(raw_path);
                entry->cmdline = kflags;
            } else if (clover_type == 2) {
                entry = std::make_shared<OutEntry>();
                entry->title = sanitize_title(title);
                entry->type = "chainload";
                entry->chain = clover_normalize_path(raw_path);
                entry->cmdline = kflags;
            } else if (clover_type == 3 && is_efi) {
                entry = std::make_shared<OutEntry>();
                entry->title = sanitize_title(title);
                entry->type = "chainload";
                entry->chain = clover_normalize_path(raw_path);
                entry->cmdline = kflags;
            } else if (is_efi) {
                entry = std::make_shared<OutEntry>();
                entry->title = sanitize_title(title);
                entry->type = "chainload";
                entry->chain = clover_normalize_path(raw_path);
                entry->cmdline = kflags;
            } else if (!kernel.empty()) {
                entry = std::make_shared<OutEntry>();
                entry->title = sanitize_title(title);
                entry->type = "linux";
                entry->vmlinuz = clover_normalize_path(kernel);
                entry->cmdline = kflags;
            } else {
                rep.warn("clover.conf: entry '" + title +
                         "' has no bootable path; skipped");
                continue;
            }

            if (!volume.empty()) {
                entry->extra_comments.push_back("clover-volume=" + volume);
            }

            std::string base = !entry->vmlinuz.empty() ? entry->vmlinuz
                             : !entry->kernel.empty()  ? entry->kernel
                                                       : entry->chain;
            entry->icon =
                guess_icon(title + " " + base_name(base), entry->type);
            cfg.index_entries.push_back(entry);
            cfg.roots.push_back(make_entry(entry));
        }
    }

    if (cfg.index_entries.empty()) {
        rep.warn("clover: no boot entries found in " + path);
    }
    return cfg;
}

}  // namespace forb
