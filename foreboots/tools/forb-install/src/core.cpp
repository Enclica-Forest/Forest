// core.cpp - Reporter, small helpers, ESP context, icon tables.
#include "forb/forb.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>

namespace fs = std::filesystem;

namespace forb {

// ===========================================================================
//  Color support
// ===========================================================================
bool Color::enabled = false;

const char* Color::red = "\033[0;31m";
const char* Color::green = "\033[0;32m";
const char* Color::yellow = "\033[0;33m";
const char* Color::cyan = "\033[0;36m";
const char* Color::bold = "\033[1m";
const char* Color::reset = "\033[0m";

bool Color::detect() {
    if (!isatty(STDERR_FILENO)) return false;
    const char* term = std::getenv("TERM");
    if (!term) return false;
    std::string t(term);
    return t != "dumb";
}

void Color::set(bool on) {
    enabled = on;
    if (on) {
        const char* no_color = std::getenv("NO_COLOR");
        if (no_color && no_color[0] != '\0') enabled = false;
    }
}

// ===========================================================================
//  Icon tables
// ===========================================================================
const std::set<std::string> ICON_NAMES = {
    "os", "text", "safe", "gear", "shield", "reboot", "ubuntu", "debian",
    "arch", "fedora", "mint", "tux", "windows", "grub", "usb", "disk",
    "terminal", "settings",
};

const std::vector<std::pair<std::string, std::string>> ICON_KEYWORDS = {
    {"windows boot", "windows"},
    {"efi fallback", "usb"},
    {"windows", "windows"},
    {"grub", "grub"},
    {"ubuntu", "ubuntu"},
    {"debian", "debian"},
    {"fedora", "fedora"},
    {"mint", "mint"},
    {"arch", "arch"},
    {"cachyos", "arch"},
    {"endeavour", "arch"},
    {"manjaro", "arch"},
    {"snapshot", "safe"},
    {"fallback", "safe"},
    {"safe", "safe"},
    {"usb", "usb"},
    {"removable", "usb"},
    {"shell", "terminal"},
};

const std::vector<Extra> EXTRAS = {
    {"ForeB Shell", "shell", "terminal"},
    {"Recovery / Disk Tools", "recovery", "gear"},
    {"Tools", "tools", "gear"},
    {"Firmware Setup (UEFI)", "setup", "settings"},
    {"Reboot", "reboot", "reboot"},
};

// ===========================================================================
//  Diagnostics
// ===========================================================================
void Reporter::warn(const std::string& msg) {
    warnings.push_back(msg);
    if (Color::enabled)
        std::cerr << Color::yellow << "warning: " << Color::reset << msg << "\n";
    else
        std::cerr << "warning: " << msg << "\n";
}

void Reporter::note(const std::string& msg) {
    notes.push_back(msg);
    if (verbose) {
        if (Color::enabled)
            std::cerr << Color::cyan << "note: " << Color::reset << msg << "\n";
        else
            std::cerr << "note: " << msg << "\n";
    }
}

void Reporter::error(const std::string& msg) {
    if (Color::enabled)
        std::cerr << Color::red << "error: " << Color::reset << msg << "\n";
    else
        std::cerr << "error: " << msg << "\n";
}

void Reporter::success(const std::string& msg) {
    if (Color::enabled)
        std::cerr << Color::green << "success: " << Color::reset << msg << "\n";
    else
        std::cerr << "success: " << msg << "\n";
}

void Reporter::verbose_out(const std::string& msg) {
    if (verbose) {
        if (Color::enabled)
            std::cerr << Color::cyan << "[verbose] " << Color::reset << msg << "\n";
        else
            std::cerr << "[verbose] " << msg << "\n";
    }
}

int die(const std::string& msg) {
    if (Color::enabled)
        std::cerr << Color::red << Color::bold << TOOL << ": error: " 
                  << Color::reset << Color::red << msg << Color::reset << "\n";
    else
        std::cerr << TOOL << ": error: " << msg << "\n";
    return 1;
}

int die(const std::string& msg, const std::string& hint) {
    if (Color::enabled)
        std::cerr << Color::red << Color::bold << TOOL << ": error: " 
                  << Color::reset << Color::red << msg << Color::reset << "\n"
                  << Color::yellow << "Hint: " << Color::reset << hint << "\n";
    else
        std::cerr << TOOL << ": error: " << msg << "\n"
                  << "Hint: " << hint << "\n";
    return 1;
}

int die_with_context(const std::string& context, const std::string& msg) {
    if (Color::enabled)
        std::cerr << Color::red << Color::bold << TOOL << ": error: " 
                  << Color::reset << Color::red << context << ": " << msg 
                  << Color::reset << "\n";
    else
        std::cerr << TOOL << ": error: " << context << ": " << msg << "\n";
    return 1;
}

// ===========================================================================
//  Progress
// ===========================================================================
void Progress::start(int total, const std::string& title) {
    if (!enabled) return;
    total_steps = total;
    current_step = 0;
    start_time_ = std::chrono::steady_clock::now();
    started_ = true;
    if (!title.empty())
        std::cerr << "\033[1;36m" << title << "\033[0m\n";
}

void Progress::step(const std::string& message) {
    if (!enabled || !started_) return;
    ++current_step;
    std::cerr << "\r\033[2K[" << current_step << "/" << total_steps << "] "
              << "\033[33m" << message << "\033[0m "
              << "(" << elapsed_str() << ")" << std::flush;
}

void Progress::finish(const std::string& message) {
    if (!enabled || !started_) return;
    std::string msg = message.empty() ? "done" : message;
    std::cerr << "\r\033[2K[" << current_step << "/" << total_steps << "] "
              << "\033[32m" << msg << "\033[0m "
              << "(" << elapsed_str() << ")\n" << std::flush;
}

void Progress::error(const std::string& message) {
    if (!enabled || !started_) return;
    std::cerr << "\r\033[2K[" << current_step << "/" << total_steps << "] "
              << "\033[31merror: " << message << "\033[0m "
              << "(" << elapsed_str() << ")\n" << std::flush;
}

std::string Progress::elapsed_str() const {
    if (!started_) return "0.0s";
    auto now = std::chrono::steady_clock::now();
    double secs =
        std::chrono::duration<double>(now - start_time_).count();
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1fs", secs);
    return buf;
}

// ===========================================================================
//  Small helpers
// ===========================================================================
std::string strip(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(
                            static_cast<unsigned char>(c)));
    return s;
}

bool starts_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

bool ends_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() &&
           s.compare(s.size() - p.size(), p.size(), p) == 0;
}

std::vector<std::string> splitlines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    // Python splitlines drops a trailing empty produced by a final newline.
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string join(const std::string& sep, const std::vector<std::string>& v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += sep;
        out += v[i];
    }
    return out;
}

std::string base_name(const std::string& path) {
    size_t pos = path.find_last_of('/');
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::string replace_all(std::string s, const std::string& from,
                        const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::string sanitize_title(const std::string& title) {
    // '/' -> U+2215 DIVISION SLASH (UTF-8 E2 88 95); '"' -> '\''.
    std::string s = replace_all(title, "/", "\xE2\x88\x95");
    s = replace_all(s, "\"", "'");
    return s;
}

std::string normalize_esp_path(std::string p) {
    p = strip(p);
    p = replace_all(p, "\\", "/");
    size_t i = 0;
    while (i < p.size() && p[i] == '/') ++i;
    return "/" + p.substr(i);
}

std::string guess_icon(const std::string& text, const std::string& type) {
    std::string hay = lower(text);
    for (const auto& kv : ICON_KEYWORDS)
        if (hay.find(kv.first) != std::string::npos) return kv.second;
    if (type == "linux") return "tux";
    if (type == "chainload") return "usb";
    if (type == "forest") return "os";
    return "";
}

std::string read_text(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string data = ss.str();
    data = replace_all(data, "\r\n", "\n");
    data = replace_all(data, "\r", "\n");
    return data;
}

void atomic_write(const std::string& path, const std::string& data) {
    fs::path p(path);
    fs::path dir = p.has_parent_path() ? p.parent_path() : fs::path(".");
    std::string tmpl = (dir / ".forb-install-XXXXXX").string();
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    int fd = ::mkstemp(buf.data());
    if (fd < 0) throw std::runtime_error("mkstemp failed for " + path);
    std::string tmp(buf.data());
    try {
        ssize_t off = 0;
        const char* d = data.data();
        size_t n = data.size();
        while (static_cast<size_t>(off) < n) {
            ssize_t w = ::write(fd, d + off, n - off);
            if (w < 0) throw std::runtime_error("write failed for " + tmp);
            off += w;
        }
        ::close(fd);
        fd = -1;
        fs::rename(tmp, path);
    } catch (...) {
        if (fd >= 0) ::close(fd);
        std::error_code ec;
        fs::remove(tmp, ec);
        throw;
    }
}

std::optional<std::string> run_cmd(const std::vector<std::string>& cmd,
                                   int /*timeout*/) {
    if (cmd.empty()) return std::nullopt;
    int pipefd[2];
    if (::pipe(pipefd) != 0) return std::nullopt;
    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return std::nullopt;
    }
    if (pid == 0) {
        ::close(pipefd[0]);
        ::dup2(pipefd[1], STDOUT_FILENO);
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) { ::dup2(devnull, STDERR_FILENO); ::close(devnull); }
        ::close(pipefd[1]);
        std::vector<char*> argv;
        for (const auto& a : cmd)
            argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        ::execvp(argv[0], argv.data());
        _exit(127);
    }
    ::close(pipefd[1]);
    std::string out;
    char buf[4096];
    ssize_t r;
    while ((r = ::read(pipefd[0], buf, sizeof buf)) > 0)
        out.append(buf, static_cast<size_t>(r));
    ::close(pipefd[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return std::nullopt;
    return out;
}

bool which(const std::string& name) {
    const char* path = std::getenv("PATH");
    if (!path) return false;
    std::string p(path);
    size_t start = 0;
    while (start <= p.size()) {
        size_t colon = p.find(':', start);
        std::string dir = p.substr(start, colon == std::string::npos
                                              ? std::string::npos
                                              : colon - start);
        if (!dir.empty()) {
            std::string cand = dir + "/" + name;
            if (::access(cand.c_str(), X_OK) == 0) return true;
        }
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return false;
}

// glob-style match supporting * ? [seq] (fnmatch.fnmatchcase semantics).
bool fnmatch_case(const std::string& name, const std::string& pat) {
    size_t n = 0, p = 0;
    size_t star_p = std::string::npos, star_n = 0;
    while (n < name.size()) {
        if (p < pat.size() && (pat[p] == '?' || pat[p] == name[n])) {
            ++p; ++n;
        } else if (p < pat.size() && pat[p] == '[') {
            size_t q = p + 1;
            bool neg = false;
            if (q < pat.size() && (pat[q] == '!' || pat[q] == '^')) {
                neg = true; ++q;
            }
            bool matched = false;
            size_t first = q;
            while (q < pat.size() && (pat[q] != ']' || q == first)) {
                if (q + 2 < pat.size() && pat[q + 1] == '-' &&
                    pat[q + 2] != ']') {
                    if (name[n] >= pat[q] && name[n] <= pat[q + 2])
                        matched = true;
                    q += 3;
                } else {
                    if (pat[q] == name[n]) matched = true;
                    ++q;
                }
            }
            if (q < pat.size() && pat[q] == ']') {
                if (matched != neg) { p = q + 1; ++n; continue; }
            }
            // malformed class or no match -> fall through to star backtrack
            if (star_p != std::string::npos) {
                p = star_p + 1; n = ++star_n;
            } else {
                return false;
            }
        } else if (p < pat.size() && pat[p] == '*') {
            star_p = p; star_n = n; ++p;
        } else if (star_p != std::string::npos) {
            p = star_p + 1; n = ++star_n;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

// ===========================================================================
//  ESP context
// ===========================================================================
std::pair<std::optional<std::string>, std::optional<std::string>>
split_disk_part(const std::string& dev) {
    static const std::regex re1(
        R"(^(/dev/nvme\d+n\d+|/dev/mmcblk\d+|/dev/loop\d+)p(\d+)$)");
    static const std::regex re2(
        R"(^(/dev/(?:sd|vd|hd|xvd)[a-z]+|/dev/[a-z]+[a-z])(\d+)$)");
    std::smatch m;
    if (std::regex_match(dev, m, re1)) return {m[1].str(), m[2].str()};
    if (std::regex_match(dev, m, re2)) return {m[1].str(), m[2].str()};
    return {std::nullopt, std::nullopt};
}

std::string EspContext::esp_file(const std::string& esp_path) const {
    std::string rel = esp_path;
    size_t i = 0;
    while (i < rel.size() && rel[i] == '/') ++i;
    return (fs::path(esp) / rel.substr(i)).string();
}

std::optional<std::set<std::string>> EspContext::partition_uuids() {
    if (uuids_tried_) return uuids_;
    uuids_tried_ = true;
    std::set<std::string> result;
    std::string dev;
    auto out = run_cmd({"findmnt", "-no", "SOURCE", esp});
    if (out && !strip(*out).empty()) {
        auto lines = splitlines(strip(*out));
        if (!lines.empty()) dev = strip(lines[0]);
    }
    if (!dev.empty()) {
        auto push_tokens = [&](const std::string& s) {
            std::istringstream iss(s);
            std::string tok;
            while (iss >> tok) result.insert(lower(tok));
        };
        out = run_cmd({"lsblk", "-no", "PARTUUID,UUID", dev});
        if (out) push_tokens(*out);
        auto dp = split_disk_part(dev);
        if (dp.first) {
            out = run_cmd({"lsblk", "-no", "PTUUID", *dp.first});
            if (out) push_tokens(*out);
        }
        const std::string byuuid = "/dev/disk/by-partuuid";
        std::error_code ec;
        if (fs::is_directory(byuuid, ec)) {
            fs::path real = fs::canonical(dev, ec);
            if (!ec) {
                for (const auto& e : fs::directory_iterator(byuuid, ec)) {
                    fs::path r = fs::canonical(e.path(), ec);
                    if (!ec && r == real)
                        result.insert(lower(e.path().filename().string()));
                }
            }
        }
    }
    if (!result.empty()) uuids_ = result;
    return uuids_;
}

std::optional<bool> EspContext::match_uuid(const std::string& arg) {
    auto uuids = partition_uuids();
    if (!uuids) return std::nullopt;
    return uuids->count(lower(strip(arg))) > 0;
}

std::optional<std::string> detect_esp() {
    for (const char* cand : {"/boot", "/boot/efi", "/efi"}) {
        auto out = run_cmd({"findmnt", "-no", "FSTYPE", cand});
        if (out && strip(*out) == "vfat") return std::string(cand);
    }
    return std::nullopt;
}

bool Node::is_entry() const {
    static const char* boot_keys[] = {"protocol", "path", "kernel_path",
                                      "image_path"};
    for (const char* k : boot_keys)
        if (keys.count(k)) return true;
    return false;
}

}  // namespace forb
