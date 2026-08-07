// forb-install - C++17 host-side installer/translator for the ForeB UEFI
// bootloader.  1:1 port of tools/forebo-install (Python) with two additions:
//   * loader install path EFI/forb/ (was EFI/ForeB/)
//   * self-contained embedded payload (BOOTX64.EFI + forebo.cfg + assets)
#ifndef FORB_FORB_HPP
#define FORB_FORB_HPP

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace forb {

constexpr const char* VERSION = "1.0.0";
constexpr const char* TOOL = "forb-install";

// Firmware limits.
constexpr int MAX_PATH = 255;
constexpr int MAX_CMDLINE = 255;
constexpr int MAX_TITLE = 63;
constexpr int MAX_ROWS = 64;
constexpr int DEFAULT_MAX_ENTRIES = 56;

extern const std::set<std::string> ICON_NAMES;
extern const std::vector<std::pair<std::string, std::string>> ICON_KEYWORDS;
struct Extra { std::string title, type, icon; };
extern const std::vector<Extra> EXTRAS;

// ---------------------------------------------------------------------------
//  Progress reporting
// ---------------------------------------------------------------------------
class Progress {
public:
    bool enabled = true;
    int total_steps = 0;
    int current_step = 0;
    std::chrono::steady_clock::time_point start_time_;
    bool started_ = false;

    explicit Progress(bool en = true) : enabled(en) {}
    void start(int total, const std::string& title = "");
    void step(const std::string& message);
    void finish(const std::string& message = "");
    void error(const std::string& message);
    std::string elapsed_str() const;

private:
    void print_prefix() const;
};

// ---------------------------------------------------------------------------
//  Color support
// ---------------------------------------------------------------------------
class Color {
public:
    static bool enabled;
    static bool detect();
    static void set(bool on);
    
    static const char* red;
    static const char* green;
    static const char* yellow;
    static const char* cyan;
    static const char* bold;
    static const char* reset;
};

// ---------------------------------------------------------------------------
//  Diagnostics
// ---------------------------------------------------------------------------
class Reporter {
public:
    bool verbose = false;
    bool force = false;
    bool quiet = false;
    std::vector<std::string> warnings;
    std::vector<std::string> notes;
    explicit Reporter(bool v = false, bool f = false) : verbose(v), force(f) {}
    void warn(const std::string& msg);
    void note(const std::string& msg);
    void error(const std::string& msg);
    void success(const std::string& msg);
    void verbose_out(const std::string& msg);
};

int die(const std::string& msg);  // prints error, returns 1
int die(const std::string& msg, const std::string& hint);  // prints error + hint
int die_with_context(const std::string& context, const std::string& msg);

// ---------------------------------------------------------------------------
//  Small helpers (util.cpp)
// ---------------------------------------------------------------------------
std::string sanitize_title(const std::string& title);
std::string normalize_esp_path(std::string p);
std::string guess_icon(const std::string& text, const std::string& type = "");
std::string read_text(const std::string& path);          // throws std::runtime_error
void atomic_write(const std::string& path, const std::string& data);
std::optional<std::string> run_cmd(const std::vector<std::string>& cmd,
                                    int timeout = 15);
bool which(const std::string& name);

std::string strip(const std::string& s);
std::string lower(std::string s);
bool starts_with(const std::string& s, const std::string& p);
bool ends_with(const std::string& s, const std::string& p);
std::vector<std::string> splitlines(const std::string& s);
std::string join(const std::string& sep, const std::vector<std::string>& v);
std::string base_name(const std::string& path);
std::string replace_all(std::string s, const std::string& from,
                        const std::string& to);
bool fnmatch_case(const std::string& name, const std::string& pat);

// ---------------------------------------------------------------------------
//  ESP context (esp.cpp)
// ---------------------------------------------------------------------------
std::pair<std::optional<std::string>, std::optional<std::string>>
split_disk_part(const std::string& dev);
std::optional<std::string> detect_esp();

class EspContext {
public:
    std::string esp;
    Reporter& reporter;
    EspContext(std::string e, Reporter& r) : esp(std::move(e)), reporter(r) {}
    std::string esp_file(const std::string& esp_path) const;
    std::optional<std::set<std::string>> partition_uuids();
    std::optional<bool> match_uuid(const std::string& arg);

private:
    std::optional<std::set<std::string>> uuids_;
    bool uuids_tried_ = false;
};

// ---------------------------------------------------------------------------
//  Data model (model)
// ---------------------------------------------------------------------------
struct Node {
    std::string name;
    int depth = 0;
    int line = 0;
    std::map<std::string, std::string> keys;
    std::vector<std::string> module_path;
    std::vector<std::shared_ptr<Node>> children;
    bool is_entry() const;
};

struct OutEntry {
    std::string title;
    std::string type;              // forest|linux|chainload
    std::string kernel;
    std::vector<std::string> modules;
    std::string vmlinuz;
    std::string initrd;
    std::string chain;
    std::string cmdline;
    std::string icon;
    std::vector<std::string> extra_comments;
    std::string source_name;
};

struct OutGroup;

struct OutNode {
    std::shared_ptr<OutGroup> group;
    std::shared_ptr<OutEntry> entry;
    bool is_group() const { return static_cast<bool>(group); }
};

struct OutGroup {
    std::string title;
    std::string icon;
    std::vector<OutNode> children;
};

inline OutNode make_group(std::shared_ptr<OutGroup> g) {
    OutNode n; n.group = std::move(g); return n;
}
inline OutNode make_entry(std::shared_ptr<OutEntry> e) {
    OutNode n; n.entry = std::move(e); return n;
}

struct DefaultSpec {
    enum Kind { None, Index, Pattern, Path } kind = None;
    int index = 0;      // for Index
    std::string str;    // for Pattern / Path
};

struct ParsedConfig {
    std::string kind;           // limine|grub|systemd-boot
    std::string source_path;
    std::optional<int> timeout;
    bool remember_last = false;
    DefaultSpec def;
    std::optional<std::string> wallpaper;      // resolved ESP-absolute
    std::optional<std::string> wallpaper_raw;
    std::vector<OutNode> roots;
    std::vector<std::shared_ptr<OutEntry>> index_entries;
};

struct BuildResult {
    std::string cfg_text;
    ParsedConfig parsed;
    std::vector<OutNode> roots;
    std::string default_str;
    std::optional<std::string> background;
    // (dest filename, content bytes)
    std::optional<std::pair<std::string, std::string>> wallpaper;
    int n_entries = 0;
    int n_submenus = 0;
    int n_rows = 0;
    std::string esp;
};

// ---------------------------------------------------------------------------
//  Config validation (lint)
// ---------------------------------------------------------------------------
struct LintMessage {
    enum Level { Error, Warning, Info } level = Info;
    std::string text;
};

struct LintResult {
    std::vector<LintMessage> messages;
    int errors() const;
    int warnings() const;
    int infos() const;
    bool has_errors() const { return errors() > 0; }
    bool has_warnings() const { return warnings() > 0; }
    void error(const std::string& msg);
    void warn(const std::string& msg);
    void info(const std::string& msg);
};

// Validate a forebo.cfg text.  If esp is non-empty, check that referenced
// files exist on disk.  If cfg is already parsed, pass it directly.
LintResult validate_config(const std::string& text,
                           const std::string& esp = "");
LintResult validate_config(const ParsedConfig& parsed,
                           const std::string& esp = "",
                           const std::string& text = "");

// ---------------------------------------------------------------------------
//  Parsers
// ---------------------------------------------------------------------------
std::string resolve_limine_path(const std::string& value, EspContext& ctx,
                                Reporter& rep, const std::string& where = "");
ParsedConfig parse_limine(const std::string& path, EspContext& ctx,
                          Reporter& rep);
ParsedConfig parse_grub(const std::string& path, EspContext& ctx,
                        Reporter& rep);
ParsedConfig parse_systemd_boot(const std::string& esp_dir,
                                const std::string& conf_path, Reporter& rep);
ParsedConfig parse_clover(const std::string& path, EspContext& ctx,
                          Reporter& rep);
ParsedConfig parse_syslinux(const std::string& path, EspContext& ctx,
                            Reporter& rep);
ParsedConfig parse_zfsbootmenu(const std::string& path, EspContext& ctx,
                               Reporter& rep);
ParsedConfig parse_refind(const std::string& path, EspContext& ctx,
                          Reporter& rep);

// ---------------------------------------------------------------------------
//  Image (image.cpp)
// ---------------------------------------------------------------------------
struct PngError { std::string msg; };
struct Image { int w = 0, h = 0; std::vector<std::string> rows; };  // rows: RGB888 top-down
int paeth(int a, int b, int c);
Image png_decode(const std::string& data);           // throws PngError
std::string bmp_encode(int w, int h, const std::vector<std::string>& rows);
std::string png_to_bmp(const std::string& data);      // throws PngError

// ---------------------------------------------------------------------------
//  Emission (emit.cpp)
// ---------------------------------------------------------------------------
std::vector<std::pair<std::shared_ptr<OutEntry>, std::vector<std::string>>>
flatten_entries(const std::vector<OutNode>& roots);
int count_submenus(const std::vector<OutNode>& roots);
std::optional<std::vector<std::string>>
find_path(const std::vector<OutNode>& roots,
          const std::shared_ptr<OutEntry>& entry);
std::vector<OutNode> cap_entries(std::vector<OutNode> roots, int max_entries,
                                 Reporter& rep);
void validate_entry(OutEntry& e, Reporter& rep);
void detect_duplicate_entries(
    const std::vector<std::shared_ptr<OutEntry>>& entries, Reporter& rep);
std::string resolve_default(const ParsedConfig& cfg,
                            const std::vector<OutNode>& roots,
                            std::optional<int> override_idx, Reporter& rep);
std::string emit_config(const ParsedConfig& cfg,
                        const std::vector<OutNode>& roots,
                        const std::string& default_str,
                        const std::optional<std::string>& background,
                        bool extras);

// ---------------------------------------------------------------------------
//  Export to other formats (commands.cpp)
// ---------------------------------------------------------------------------
std::string export_to_grub(const ParsedConfig& cfg,
                           const std::vector<OutNode>& roots);
std::string export_to_limine(const ParsedConfig& cfg,
                             const std::vector<OutNode>& roots);
std::string export_to_systemd(const ParsedConfig& cfg,
                              const std::vector<OutNode>& roots);
std::string export_to_syslinux(const ParsedConfig& cfg,
                               const std::vector<OutNode>& roots);

// ---------------------------------------------------------------------------
//  Bootloader detection + migration
// ---------------------------------------------------------------------------
struct BootloaderInfo {
    std::string name;            // "grub", "limine", "systemd-boot", etc.
    double confidence = 0.0;     // 0.0 – 1.0
    std::string config_path;     // path to the detected config file
    std::vector<std::string> features;   // what can be migrated
    std::vector<std::string> warnings;   // incompatible features
    std::string migration_path;  // best migration suggestion
};

std::vector<BootloaderInfo> detect_bootloader(const std::string& dir);

// ---------------------------------------------------------------------------
//  Build pipeline + commands (build.cpp / commands.cpp)
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
//  Batch manifest
// ---------------------------------------------------------------------------
struct BatchEntry {
    std::string type;    // "foreb", "grub", "limine", "systemd-boot", etc.
    std::string config;  // source config path (absolute or ESP-relative)
    std::string esp;     // ESP mount point
};

struct BatchManifest {
    std::vector<BatchEntry> bootloaders;
};

BatchManifest parse_batch_manifest(const std::string& path);

struct Args {
    std::string command;          // "", scan, generate, install, migrate, export
    bool selftest = false;
    std::string esp;
    std::string config;
    std::string repo;             // "" = use embedded payload
    std::optional<int> default_entry;
    int max_entries = DEFAULT_MAX_ENTRIES;
    bool no_extras = false;
    bool verbose = false;
    bool quiet = false;
    bool force = false;
    bool color = false;
    bool no_color = false;
    bool strict = false;          // fail on warnings
    std::string output;           // generate, export
    bool no_nvram = false;        // install
    bool make_default = false;    // install
    bool dry_run = false;         // install / migrate
    std::string source;           // migrate: source path to scan
    bool backup = false;          // migrate: back up original config
    std::string export_format;    // export: grub|limine|systemd-boot|syslinux
    bool yes = false;             // uninstall: skip confirmation
    bool keep_nvram = false;      // uninstall: keep NVRAM entry
    bool clean = false;           // install: remove old ForeB before installing
    std::string manifest;         // batch: manifest file
    bool continue_on_error = false; // batch: continue on error
};

std::vector<std::pair<std::string, std::string>> autodetect_config(
    const std::string& esp);
std::string infer_kind(const std::string& path);
std::optional<BuildResult> build_config(const Args& args, Reporter& rep);

int cmd_scan(const Args& args, Reporter& rep);
int cmd_generate(const Args& args, Reporter& rep);
int cmd_install(const Args& args, Reporter& rep);
int cmd_lint(const Args& args, Reporter& rep);
int cmd_uninstall(const Args& args, Reporter& rep);
int cmd_list(const Args& args, Reporter& rep);
int cmd_backup(const Args& args, Reporter& rep);
int cmd_migrate(const Args& args, Reporter& rep);
int cmd_batch(const Args& args, Reporter& rep);
int cmd_export(const Args& args, Reporter& rep);
std::string safe_rel_path(const std::string& rel);  // throws std::runtime_error

// ---------------------------------------------------------------------------
//  Payload (payload.cpp)
// ---------------------------------------------------------------------------
struct PayloadFile {
    std::string name;             // ESP-relative, e.g. EFI/forb/BOOTX64.EFI
    std::string data;
};
const std::vector<PayloadFile>& payload_files();

// ---------------------------------------------------------------------------
//  mkrescue (mkrescue.cpp) - `forb-mkrescue`, drop-in for grub-mkrescue.
// ---------------------------------------------------------------------------
int cmd_mkrescue(const std::vector<std::string>& argv);  // argv AFTER the command

// ---------------------------------------------------------------------------
//  Selftest (selftest.cpp)
// ---------------------------------------------------------------------------
int cmd_selftest();

}  // namespace forb

#endif  // FORB_FORB_HPP
