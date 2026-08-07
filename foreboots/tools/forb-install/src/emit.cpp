// emit.cpp - forebo.cfg emission + entry validation + default resolution.
#include "forb/forb.hpp"

#include <ctime>
#include <functional>

namespace forb {

std::vector<std::pair<std::shared_ptr<OutEntry>, std::vector<std::string>>>
flatten_entries(const std::vector<OutNode>& roots) {
    std::vector<std::pair<std::shared_ptr<OutEntry>,
                          std::vector<std::string>>> res;
    std::function<void(const std::vector<OutNode>&, std::vector<std::string>)>
        walk = [&](const std::vector<OutNode>& nodes,
                   std::vector<std::string> path) {
            for (const auto& n : nodes) {
                if (n.is_group()) {
                    auto p2 = path;
                    p2.push_back(n.group->title);
                    walk(n.group->children, p2);
                } else {
                    res.emplace_back(n.entry, path);
                }
            }
        };
    walk(roots, {});
    return res;
}

int count_submenus(const std::vector<OutNode>& roots) {
    int total = 0;
    std::function<void(const std::vector<OutNode>&)> walk =
        [&](const std::vector<OutNode>& nodes) {
            for (const auto& n : nodes) {
                if (n.is_group()) { ++total; walk(n.group->children); }
            }
        };
    walk(roots);
    return total;
}

std::optional<std::vector<std::string>>
find_path(const std::vector<OutNode>& roots,
          const std::shared_ptr<OutEntry>& entry) {
    for (auto& [e, path] : flatten_entries(roots)) {
        if (e == entry) {
            auto p = path;
            p.push_back(e->title);
            return p;
        }
    }
    return std::nullopt;
}

std::vector<OutNode> cap_entries(std::vector<OutNode> roots, int max_entries,
                                 Reporter& rep) {
    int count = 0;
    std::function<std::vector<OutNode>(std::vector<OutNode>)> filt =
        [&](std::vector<OutNode> nodes) -> std::vector<OutNode> {
            std::vector<OutNode> out;
            for (auto& n : nodes) {
                if (n.is_group()) {
                    n.group->children = filt(n.group->children);
                    if (!n.group->children.empty()) out.push_back(n);
                    else rep.note("submenu '" + n.group->title + "' left "
                                  "empty by --max-entries cap; dropped");
                } else {
                    ++count;
                    if (count <= max_entries) out.push_back(n);
                    else rep.warn("entry '" + n.entry->title + "' dropped: "
                                  "--max-entries cap (" +
                                  std::to_string(max_entries) + ") reached");
                }
            }
            return out;
        };
    return filt(std::move(roots));
}

void validate_entry(OutEntry& e, Reporter& rep) {
    if (static_cast<int>(e.title.size()) > MAX_TITLE) {
        rep.warn("title '" + e.title + "' is " +
                 std::to_string(e.title.size()) + " chars (firmware max " +
                 std::to_string(MAX_TITLE) + "); truncated");
        e.title = e.title.substr(0, MAX_TITLE);
    }
    struct LV { const char* label; const std::string& val; };
    for (const LV& lv : {LV{"kernel", e.kernel}, LV{"vmlinuz", e.vmlinuz},
                         LV{"initrd", e.initrd}, LV{"chain", e.chain}}) {
        if (static_cast<int>(lv.val.size()) > MAX_PATH)
            rep.warn(std::string("entry '") + e.title + "': " + lv.label +
                     " path is " + std::to_string(lv.val.size()) +
                     " chars (firmware max " + std::to_string(MAX_PATH) +
                     "); firmware will truncate it");
    }
    for (const auto& m : e.modules) {
        if (static_cast<int>(m.size()) > MAX_PATH)
            rep.warn("entry '" + e.title + "': module path is " +
                     std::to_string(m.size()) + " chars (firmware max " +
                     std::to_string(MAX_PATH) + "); firmware will truncate it");
    }
    if (e.cmdline.find('"') != std::string::npos) {
        rep.note("entry '" + e.title + "': '\"' in cmdline replaced by \"'\"");
        e.cmdline = replace_all(e.cmdline, "\"", "'");
    }
    if (static_cast<int>(e.cmdline.size()) > MAX_CMDLINE)
        rep.warn("entry '" + e.title + "': cmdline is " +
                 std::to_string(e.cmdline.size()) + " chars (firmware max " +
                 std::to_string(MAX_CMDLINE) + "); firmware will truncate it");
}

std::string resolve_default(const ParsedConfig& cfg,
                            const std::vector<OutNode>& roots,
                            std::optional<int> override_idx, Reporter& rep) {
    auto entries = flatten_entries(roots);
    DefaultSpec spec = cfg.def;
    if (override_idx.has_value()) {
        spec.kind = DefaultSpec::Index;
        spec.index = *override_idx;
    }
    if (spec.kind == DefaultSpec::None) {
        rep.note("no default entry configured; emitting default=0");
        return "0";
    }
    if (spec.kind == DefaultSpec::Index) {
        int idx = spec.index;
        if (idx < 1 || idx > static_cast<int>(cfg.index_entries.size())) {
            rep.note("default entry " + std::to_string(idx) +
                     " out of range (" +
                     std::to_string(cfg.index_entries.size()) +
                     " entries); emitting default=0");
            return "0";
        }
        auto target = cfg.index_entries[idx - 1];
        if (!target) {
            rep.warn("default entry " + std::to_string(idx) + " was skipped "
                     "during translation (unsupported); emitting default=0");
            return "0";
        }
        auto path = find_path(roots, target);
        if (!path) {
            rep.warn("default entry " + std::to_string(idx) + " fell victim "
                     "to the --max-entries cap; emitting default=0");
            return "0";
        }
        return join("/", *path);
    }
    if (spec.kind == DefaultSpec::Pattern) {
        const std::string& pat = spec.str;
        for (auto& [e, path] : entries) {
            std::string stem = ends_with(e->source_name, ".conf")
                ? e->source_name.substr(0, e->source_name.size() - 5)
                : e->source_name;
            std::vector<std::string> names = {e->source_name, stem, e->title};
            for (const auto& nm : names) {
                if (!nm.empty() && (nm == pat || fnmatch_case(nm, pat))) {
                    auto p = path;
                    p.push_back(e->title);
                    return join("/", p);
                }
            }
        }
        rep.note("default pattern '" + pat +
                 "' matched no entry; emitting default=0");
        return "0";
    }
    // Path
    return spec.str;
}

static void emit_entry(const OutEntry& e, const std::string& ind,
                       std::vector<std::string>& out) {
    out.push_back(ind + "menuentry \"" + e.title + "\" {");
    out.push_back(ind + "    type=" + e.type);
    if (e.type == "forest") {
        out.push_back(ind + "    kernel=" + e.kernel);
        for (const auto& m : e.modules)
            out.push_back(ind + "    module=" + m);
        for (const auto& c : e.extra_comments)
            out.push_back(ind + "    # extra module (unsupported): " + c);
    } else if (e.type == "linux") {
        out.push_back(ind + "    vmlinuz=" + e.vmlinuz);
        if (!e.initrd.empty())
            out.push_back(ind + "    initrd=" + e.initrd);
        for (const auto& c : e.extra_comments)
            out.push_back(ind + "    # extra module (unsupported): " + c);
    } else if (e.type == "chainload") {
        if (!e.chain.empty())
            out.push_back(ind + "    chain=" + e.chain);
    }
    if (e.type == "forest" || e.type == "linux" || !e.cmdline.empty())
        out.push_back(ind + "    cmdline=\"" + e.cmdline + "\"");
    if (!e.icon.empty())
        out.push_back(ind + "    icon=" + e.icon);
    out.push_back(ind + "}");
    out.push_back("");
}

static void emit_nodes(const std::vector<OutNode>& nodes,
                       const std::string& ind, std::vector<std::string>& out) {
    for (const auto& n : nodes) {
        if (n.is_group()) {
            out.push_back(ind + "submenu \"" + n.group->title + "\" {");
            if (!n.group->icon.empty())
                out.push_back(ind + "    icon=" + n.group->icon);
            emit_nodes(n.group->children, ind + "    ", out);
            if (!out.empty() && out.back().empty()) out.pop_back();
            out.push_back(ind + "}");
            out.push_back("");
        } else {
            emit_entry(*n.entry, ind, out);
        }
    }
}

std::string emit_config(const ParsedConfig& cfg,
                        const std::vector<OutNode>& roots,
                        const std::string& default_str,
                        const std::optional<std::string>& background,
                        bool extras) {
    std::time_t t = std::time(nullptr);
    char stamp[64];
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S",
                  std::localtime(&t));
    std::string bar(74, '=');
    std::vector<std::string> out;
    out.push_back("# " + bar);
    out.push_back(std::string("#  forebo.cfg - generated by ") + TOOL +
                  " v" + VERSION);
    out.push_back("#  Source config : " + cfg.source_path + " (" + cfg.kind +
                  ")");
    out.push_back(std::string("#  Generated     : ") + stamp);
    if (cfg.kind == "grub")
        out.push_back("#  NOTE: grub.cfg parsing is best-effort; verify "
                      "paths.");
    out.push_back("#  Unknown keys are ignored by the ForeB firmware parser.");
    out.push_back("# " + bar);
    out.push_back("");
    if (cfg.timeout.has_value())
        out.push_back("timeout=" + std::to_string(*cfg.timeout));
    out.push_back("default=" + default_str);
    if (cfg.remember_last) out.push_back("remember_last=1");
    if (background) out.push_back("background=" + *background);
    out.push_back("");
    emit_nodes(roots, "", out);
    if (extras) {
        out.push_back("# ---- ForeB utilities ----");
        for (const auto& ex : EXTRAS) {
            out.push_back("menuentry \"" + ex.title + "\" {");
            out.push_back("    type=" + ex.type);
            out.push_back("    icon=" + ex.icon);
            out.push_back("}");
            out.push_back("");
        }
    }
    std::string joined = join("\n", out);
    // rstrip("\n") + "\n"
    size_t end = joined.size();
    while (end > 0 && joined[end - 1] == '\n') --end;
    return joined.substr(0, end) + "\n";
}

}  // namespace forb
