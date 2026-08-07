// mkrescue.cpp - `forb-mkrescue`: make a bootable ForeB rescue image
// (CD / USB / disk), a drop-in stand-in for grub-mkrescue. The ForeB EFI
// payload is embedded in this binary (payload_files()), so the produced image
// is self-contained - no repo or install tree required.
//
// grub-mkrescue-compatible options are accepted; the ForeB-relevant ones are
// honored and the rest are ignored or (like grub) passed through to xorriso.
// Any non-option SOURCE argument that exists on disk is added to the ISO root,
// exactly as grub-mkrescue folds extra source trees into the image.
#include "forb/forb.hpp"

#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;
namespace forb {

static bool has_prefix(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}
// Split "--opt=val" -> {"--opt","val"}; returns whether an '=' value was present.
static bool split_eq(const std::string& s, std::string& key, std::string& val) {
    auto e = s.find('=');
    if (e == std::string::npos) { key = s; val.clear(); return false; }
    key = s.substr(0, e); val = s.substr(e + 1); return true;
}

static bool write_disk_file(const fs::path& p, const std::string& data) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f.write(data.data(), (std::streamsize)data.size());
    return (bool)f;
}

int cmd_mkrescue(const std::vector<std::string>& argv) {
    std::string output, directory, xorriso = "xorriso";
    std::string product = "FOREB", version = "";   // matches the Makefile's -V FOREB
    bool verbose = false;
    std::vector<std::string> sources;    // extra ISO-root trees (SOURCE...)
    std::vector<std::string> passthru;   // unrecognized opts -> xorriso

    for (size_t i = 0; i < argv.size(); ++i) {
        std::string a = argv[i], key, val;
        bool eq = split_eq(a, key, val);
        auto next = [&](const std::string& cur) -> std::string {
            if (eq) return val;
            if (i + 1 < argv.size()) return argv[++i];
            fprintf(stderr, "forb-mkrescue: option %s needs an argument\n", cur.c_str());
            return "";
        };
        if (key == "-o" || key == "--output")            output = next(key);
        else if (key == "-d" || key == "--directory")    directory = next(key);
        else if (key == "--xorriso")                     xorriso = next(key);
        else if (key == "--product-name")                product = next(key);
        else if (key == "--product-version")             version = next(key);
        else if (key == "-v" || key == "--verbose")      verbose = true;
        // Accepted-but-not-applicable grub options (swallow their value if any).
        else if (key == "--compress" || key == "--core-compress" ||
                 key == "--fonts" || key == "--themes" || key == "--locales" ||
                 key == "--modules" || key == "--install-modules" ||
                 key == "--sbat" || key == "--dtb" || key == "--pubkey" ||
                 key == "-k" || key == "--x509key" || key == "-x" ||
                 key == "--locale-directory" || key == "--rom-directory" ||
                 key == "--label-bgcolor" || key == "--label-color" ||
                 key == "--label-font" || key == "--appended-signature-size") {
            (void)next(key);
        }
        else if (key == "--disable-cli" || key == "--disable-shim-lock" ||
                 key == "--arcs-boot" || key == "--sparc-boot") { /* no-op */ }
        else if (a == "--") { for (++i; i < argv.size(); ++i) passthru.push_back(argv[i]); }
        else if (has_prefix(a, "-"))  passthru.push_back(a);        // -> xorriso
        else if (fs::exists(a))       sources.push_back(a);          // SOURCE tree
        else passthru.push_back(a);                                  // -> xorriso
    }

    if (output.empty())
        return die("forb-mkrescue: -o/--output FILE is required");
    if (!directory.empty() && verbose)
        printf("note: --directory is ignored; using the embedded ForeB payload\n");

    // --- staging ----------------------------------------------------------
    fs::path tmp = fs::temp_directory_path() /
                   ("forb-mkrescue-" + std::to_string((long)::getpid()));
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp / "espsrc", ec);
    fs::create_directories(tmp / "iso" / "boot" / "efi", ec);

    // Write the embedded payload (ESP-relative tree) to disk. Ensure the loader
    // lands at the firmware-standard removable path EFI/BOOT/BOOTX64.EFI.
    size_t total = 0; bool have_efi = false;
    for (const auto& pf : payload_files()) {
        std::string name = pf.name;
        if (name.find("BOOTX64.EFI") != std::string::npos) {
            name = "EFI/BOOT/BOOTX64.EFI"; have_efi = true;
        }
        if (!write_disk_file(tmp / "espsrc" / name, pf.data))
            return die("forb-mkrescue: cannot stage payload file " + name);
        total += pf.data.size();
    }
    if (!have_efi)
        return die("forb-mkrescue: embedded payload has no BOOTX64.EFI");

    // --- build the FAT ESP image -----------------------------------------
    long kib = (long)(total / 1024) + 2048;          // payload + ~2 MiB slack
    kib = ((kib + 63) / 64) * 64;                     // round to 64 KiB
    fs::path esp = tmp / "iso" / "boot" / "efi" / "esp.img";
    if (!run_cmd({"mkfs.fat", "-C", "-F", "16", "-n", "FOREB",
                  esp.string(), std::to_string(kib)}, 60))
        return die("forb-mkrescue: mkfs.fat failed (is dosfstools installed?)");
    // Copy the whole staged tree into the ESP, recursively (mcopy -s).
    for (auto& e : fs::directory_iterator(tmp / "espsrc")) {
        if (!run_cmd({"mcopy", "-s", "-Q", "-i", esp.string(),
                      e.path().string(), "::"}, 60))
            return die("forb-mkrescue: mcopy into ESP failed (mtools installed?)");
    }

    // --- fold any user SOURCE trees into the ISO root --------------------
    for (const auto& s : sources) {
        fs::path src(s), dst = tmp / "iso" / src.filename();
        fs::copy(src, dst, fs::copy_options::recursive |
                            fs::copy_options::overwrite_existing, ec);
        if (verbose) printf("added source: %s -> /%s\n",
                            s.c_str(), src.filename().string().c_str());
    }

    // --- xorriso: UEFI El Torito + isohybrid GPT (CD + USB bootable) ------
    std::string volid = product;
    if (!version.empty()) volid += "-" + version;
    if (volid.size() > 32) volid.resize(32);
    std::vector<std::string> x = { xorriso, "-as", "mkisofs", "-V", volid,
        "-e", "boot/efi/esp.img", "-no-emul-boot",
        "-isohybrid-gpt-basdat", "-o", output };
    for (const auto& p : passthru) x.push_back(p);
    x.push_back((tmp / "iso").string());

    if (verbose) { printf("running:"); for (auto& s : x) printf(" %s", s.c_str()); printf("\n"); }
    bool ok = (bool)run_cmd(x, 300);
    if (!ok) {
        // Retry without the isohybrid GPT flag for older xorriso builds.
        std::vector<std::string> x2;
        for (auto& s : x) if (s != "-isohybrid-gpt-basdat") x2.push_back(s);
        ok = (bool)run_cmd(x2, 300);
    }
    fs::remove_all(tmp, ec);
    if (!ok) return die("forb-mkrescue: xorriso failed to build the image");

    printf("forb-mkrescue: wrote bootable ForeB rescue image -> %s\n", output.c_str());
    printf("  boot it: qemu-system-x86_64 -cdrom %s -bios /usr/share/edk2/x64/OVMF_CODE.4m.fd\n", output.c_str());
    printf("  or write to USB: dd if=%s of=/dev/sdX bs=4M status=progress && sync\n", output.c_str());
    return 0;
}

}  // namespace forb
