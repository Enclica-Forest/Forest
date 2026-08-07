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
"  batch      install multiple bootloaders from a JSON manifest\n"
"  lint       validate a forebo.cfg file for correctness\n"
"  uninstall  remove ForeB from the ESP and NVRAM (needs root)\n"
"  list       list all bootloaders detected on the ESP and in NVRAM\n"
"  backup     create a tar.gz backup of the ESP configuration\n"
"  export     export forebo.cfg to another bootloader format\n"
"\n"
"Global:\n"
"  --selftest            run internal offline tests and exit\n"
"  --force               bypass confirmation prompts, overwrite existing\n"
"  --color               force colored output\n"
"  --no-color            disable colored output\n"
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
"  --strict              treat warnings as errors\n"
"  -v, --verbose         also print notes\n"
"  -q, --quiet           suppress all output except errors\n"
"\n"
"generate options:\n"
"  --output FILE         write to FILE instead of stdout\n"
"\n"
"install options:\n"
"  --clean               remove old ForeB installation before installing\n"
"  --no-nvram            skip efibootmgr NVRAM registration\n"
"  --make-default        put ForeB first in BootOrder\n"
"  --dry-run             print every action without doing it\n"
"\n"
"batch options:\n"
"  --manifest FILE       path to JSON manifest (required)\n"
"  --continue-on-error   continue installing other bootloaders after failure\n"
"\n"
"uninstall options:\n"
"  --dry-run             print what would be removed without doing it\n"
"  --keep-nvram          do not remove the NVRAM boot entry\n"
"  --yes                 skip confirmation prompt\n"
"\n"
"list options:\n"
"  (uses common --esp option)\n"
"\n"
"backup options:\n"
"  --output FILE         save backup to FILE (default: forb-backup-TIMESTAMP.tar.gz)\n"
"\n"
"migrate options:\n"
"  --source PATH         source directory or config to migrate (auto-detect\n"
"                        ESP if omitted)\n"
"  --backup              back up original config before overwriting\n"
"  --output FILE         write forebo.cfg to FILE (default: ESP forebo/forebo.cfg)\n"
"  --dry-run             show what would be done without doing it\n"
"\n"
"export options:\n"
"  --format FORMAT       target format: grub, limine, systemd-boot, syslinux\n"
"  --output FILE         write exported config to FILE\n"
"\n"
"Environment variables:\n"
"  FORB_ESP              override default ESP path\n"
"  NO_COLOR              disable colored output (https://no-color.org/)\n"
"  TERM                  terminal type (dumb terminals get no color)\n"
"\n"
"Common usage patterns:\n"
"  " << TOOL << " scan                           # see what's on the ESP\n"
"  " << TOOL << " generate -o forebo.cfg         # preview translated config\n"
"  " << TOOL << " install --dry-run              # preview install actions\n"
"  " << TOOL << " install --clean                # remove old ForeB, then install\n"
"  " << TOOL << " install --force                # overwrite existing install\n"
"  " << TOOL << " install --make-default         # make ForeB the default\n"
"\n"
"Examples:\n"
"  " << TOOL << " scan --esp /boot/efi\n"
"  " << TOOL << " generate --config /boot/efi/grub/grub.cfg\n"
"  " << TOOL << " install --esp /boot/efi --clean --make-default\n"
"\n"
"exit codes: 0 ok, 1 error, 2 usage\n";
}

// returns false + prints error on a missing value.
static bool take_value(const std::vector<std::string>& argv, size_t& i,
                       const std::string& flag, std::string& out) {
    if (i + 1 >= argv.size()) {
        if (Color::enabled)
            std::cerr << Color::red << Color::bold << TOOL << ": error: " 
                      << Color::reset << Color::red << flag 
                      << " requires a value" << Color::reset << "\n";
        else
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

    // Initialize color based on --color/--no-color or TTY detection
    for (const auto& s : args) {
        if (s == "--color") a.color = true;
        if (s == "--no-color") a.no_color = true;
    }
    
    if (a.color) Color::set(true);
    else if (a.no_color) Color::set(false);
    else Color::set(Color::detect());

    size_t i = 0;
    // subcommand is the first non-option token.
    for (; i < args.size(); ++i) {
        const std::string& s = args[i];
        if (s == "scan" || s == "generate" || s == "install" || s == "batch" ||
            s == "lint" || s == "uninstall" || s == "migrate" || s == "list" || s == "backup" ||
            s == "export") {
            a.command = s;
            ++i;
            break;
        }
        if (s == "-h" || s == "--help") { print_help(); return 0; }
        if (Color::enabled)
            std::cerr << Color::red << Color::bold << TOOL << ": error: " 
                      << Color::reset << Color::red 
                      << "unknown token before command: " << s 
                      << Color::reset << "\n";
        else
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
                if (Color::enabled)
                    std::cerr << Color::red << Color::bold << TOOL << ": error: " 
                              << Color::reset << Color::red 
                              << "--default-entry expects an integer" 
                              << Color::reset << "\n";
                else
                    std::cerr << TOOL << ": error: --default-entry expects an "
                                         "integer\n";
                return 2;
            }
        } else if (s == "--max-entries") {
            if (!take_value(args, i, s, val)) return 2;
            try { a.max_entries = std::stoi(val); }
            catch (...) {
                if (Color::enabled)
                    std::cerr << Color::red << Color::bold << TOOL << ": error: " 
                              << Color::reset << Color::red 
                              << "--max-entries expects an integer" 
                              << Color::reset << "\n";
                else
                    std::cerr << TOOL << ": error: --max-entries expects an "
                                         "integer\n";
                return 2;
            }
        } else if (s == "--no-extras") {
            a.no_extras = true;
        } else if (s == "--strict") {
            a.strict = true;
        } else if (s == "--force") {
            a.force = true;
        } else if (s == "--clean" && (a.command == "install" || a.command == "batch")) {
            a.clean = true;
        } else if (s == "--color") {
            // Already handled above
        } else if (s == "--no-color") {
            // Already handled above
        } else if (s == "-v" || s == "--verbose") {
            a.verbose = true;
        } else if (s == "-q" || s == "--quiet") {
            a.quiet = true;
        } else if (s == "--manifest" && a.command == "batch") {
            if (!take_value(args, i, s, a.manifest)) return 2;
        } else if (s == "--continue-on-error" && a.command == "batch") {
            a.continue_on_error = true;
        } else if (s == "--output" && (a.command == "generate" || a.command == "backup" || a.command == "export" || a.command == "migrate")) {
            if (!take_value(args, i, s, a.output)) return 2;
        } else if (s == "--source" && a.command == "migrate") {
            if (!take_value(args, i, s, a.source)) return 2;
        } else if (s == "--backup" && a.command == "migrate") {
            a.backup = true;
        } else if (s == "--format" && a.command == "export") {
            if (!take_value(args, i, s, a.export_format)) return 2;
        } else if (s == "--no-nvram" && (a.command == "install" || a.command == "batch")) {
            a.no_nvram = true;
        } else if (s == "--make-default" && (a.command == "install" || a.command == "batch")) {
            a.make_default = true;
        } else if (s == "--dry-run" && (a.command == "install" || a.command == "uninstall" || a.command == "migrate" || a.command == "batch")) {
            a.dry_run = true;
        } else if (s == "--yes" && a.command == "uninstall") {
            a.yes = true;
        } else if (s == "--keep-nvram" && a.command == "uninstall") {
            a.keep_nvram = true;
        } else if (s == "-h" || s == "--help") {
            print_help();
            return 0;
        } else if (s[0] != '-' && a.command == "lint" && a.config.empty()) {
            a.config = s;
        } else {
            if (Color::enabled)
                std::cerr << Color::red << Color::bold << TOOL << ": error: " 
                          << Color::reset << Color::red 
                          << "unrecognized option for " << a.command << ": " << s 
                          << Color::reset << "\n";
            else
                std::cerr << TOOL << ": error: unrecognized option for "
                          << a.command << ": " << s << "\n";
            return 2;
        }
    }

    Reporter rep(a.verbose, a.force);
    rep.quiet = a.quiet;
    if (a.command == "scan") return cmd_scan(a, rep);
    if (a.command == "generate") return cmd_generate(a, rep);
    if (a.command == "install") return cmd_install(a, rep);
    if (a.command == "batch") return cmd_batch(a, rep);
    if (a.command == "lint") return cmd_lint(a, rep);
    if (a.command == "uninstall") return cmd_uninstall(a, rep);
    if (a.command == "migrate") return cmd_migrate(a, rep);
    if (a.command == "list") return cmd_list(a, rep);
    if (a.command == "backup") return cmd_backup(a, rep);
    if (a.command == "export") return cmd_export(a, rep);
    print_help();
    return 2;
}
