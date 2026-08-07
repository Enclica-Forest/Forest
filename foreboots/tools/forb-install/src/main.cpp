// main.cpp - hand-rolled CLI for forb-install.
#include "forb/forb.hpp"

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace forb;

static void print_help() {
    std::cout <<
"usage: " << TOOL << " [--selftest] COMMAND [options]\n"
"\n"
"Translate an existing bootloader config (Limine, GRUB, systemd-boot) into\n"
"ForeB's forebo.cfg and optionally install ForeB alongside it.\n"
"\n"
"Commands:\n"
"  scan       parse the ESP configs and print a report (read-only)\n"
"  generate   print the translated forebo.cfg (--output FILE)\n"
"  install    install ForeB alongside the current bootloader (needs root)\n"
"\n"
"Global:\n"
"  --selftest            run internal offline tests and exit\n"
"\n"
"Common options:\n"
"  --esp PATH            ESP mount point (default: first vfat mount of\n"
"                        /boot, /boot/efi, /efi)\n"
"  --config FILE         explicit source config file (skips auto-detection)\n"
"  --repo DIR            override embedded payload files from a repo tree\n"
"  --default-entry N     override the default entry (1-based, file order)\n"
"  --max-entries N       safety cap on translated entries (default "
      << DEFAULT_MAX_ENTRIES << ")\n"
"  --no-extras           do not append the ForeB utility entries\n"
"  -v, --verbose         also print notes\n"
"\n"
"generate options:\n"
"  --output FILE         write to FILE instead of stdout\n"
"\n"
"install options:\n"
"  --no-nvram            skip efibootmgr NVRAM registration\n"
"  --make-default        put ForeB first in BootOrder\n"
"  --dry-run             print every action without doing it\n"
"\n"
"exit codes: 0 ok, 1 error, 2 usage\n";
}

// returns false + prints error on a missing value.
static bool take_value(const std::vector<std::string>& argv, size_t& i,
                       const std::string& flag, std::string& out) {
    if (i + 1 >= argv.size()) {
        std::cerr << TOOL << ": error: " << flag << " requires a value\n";
        return false;
    }
    out = argv[++i];
    return true;
}

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    Args a;

    // Drop-in mode: invoked as `forb-mkrescue` (argv0 basename) OR first token
    // is `mkrescue` -> the grub-mkrescue-compatible rescue-image maker.
    {
        std::string prog = argv[0] ? argv[0] : "";
        auto slash = prog.find_last_of('/');
        if (slash != std::string::npos) prog = prog.substr(slash + 1);
        if (prog.find("mkrescue") != std::string::npos)
            return cmd_mkrescue(args);
        if (!args.empty() && args[0] == "mkrescue")
            return cmd_mkrescue(std::vector<std::string>(args.begin() + 1, args.end()));
    }

    // First pass: find --selftest anywhere / the subcommand.
    for (const auto& s : args)
        if (s == "--selftest") { a.selftest = true; }
    if (a.selftest) return cmd_selftest();

    size_t i = 0;
    // subcommand is the first non-option token.
    for (; i < args.size(); ++i) {
        const std::string& s = args[i];
        if (s == "scan" || s == "generate" || s == "install") {
            a.command = s;
            ++i;
            break;
        }
        if (s == "-h" || s == "--help") { print_help(); return 0; }
        std::cerr << TOOL << ": error: unknown token before command: " << s
                  << "\n";
        return 2;
    }
    if (a.command.empty()) { print_help(); return 2; }

    for (; i < args.size(); ++i) {
        const std::string& s = args[i];
        std::string val;
        if (s == "--esp") { if (!take_value(args, i, s, a.esp)) return 2; }
        else if (s == "--config") {
            if (!take_value(args, i, s, a.config)) return 2;
        } else if (s == "--repo") {
            if (!take_value(args, i, s, a.repo)) return 2;
        } else if (s == "--default-entry") {
            if (!take_value(args, i, s, val)) return 2;
            try { a.default_entry = std::stoi(val); }
            catch (...) {
                std::cerr << TOOL << ": error: --default-entry expects an "
                                     "integer\n";
                return 2;
            }
        } else if (s == "--max-entries") {
            if (!take_value(args, i, s, val)) return 2;
            try { a.max_entries = std::stoi(val); }
            catch (...) {
                std::cerr << TOOL << ": error: --max-entries expects an "
                                     "integer\n";
                return 2;
            }
        } else if (s == "--no-extras") {
            a.no_extras = true;
        } else if (s == "-v" || s == "--verbose") {
            a.verbose = true;
        } else if (s == "--output" && a.command == "generate") {
            if (!take_value(args, i, s, a.output)) return 2;
        } else if (s == "--no-nvram" && a.command == "install") {
            a.no_nvram = true;
        } else if (s == "--make-default" && a.command == "install") {
            a.make_default = true;
        } else if (s == "--dry-run" && a.command == "install") {
            a.dry_run = true;
        } else if (s == "-h" || s == "--help") {
            print_help();
            return 0;
        } else {
            std::cerr << TOOL << ": error: unrecognized option for "
                      << a.command << ": " << s << "\n";
            return 2;
        }
    }

    Reporter rep(a.verbose);
    if (a.command == "scan") return cmd_scan(a, rep);
    if (a.command == "generate") return cmd_generate(a, rep);
    if (a.command == "install") return cmd_install(a, rep);
    print_help();
    return 2;
}
