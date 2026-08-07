// payload.cpp - embedded ustar payload reader.
//
// The build generates payload_blob.c defining forb_payload_tar[] /
// forb_payload_tar_len (a ustar archive of the ESP-relative install tree).
// This parses it lazily into a list of ESP-relative files.
#include "forb/forb.hpp"

#include <cstring>

extern "C" {
extern const unsigned char forb_payload_tar[];
extern const unsigned long forb_payload_tar_len;
}

namespace forb {

static std::string strip_dot_slash(std::string name) {
    while (name.size() >= 2 && name[0] == '.' && name[1] == '/')
        name = name.substr(2);
    while (!name.empty() && name[0] == '/') name = name.substr(1);
    return name;
}

static const std::vector<PayloadFile>& parse_payload() {
    static std::vector<PayloadFile> files;
    static bool done = false;
    if (done) return files;
    done = true;

    const unsigned char* data = forb_payload_tar;
    size_t len = static_cast<size_t>(forb_payload_tar_len);
    size_t pos = 0;
    while (pos + 512 <= len) {
        const unsigned char* hdr = data + pos;
        // Two consecutive zero blocks terminate the archive.
        bool all_zero = true;
        for (int i = 0; i < 512; ++i)
            if (hdr[i] != 0) { all_zero = false; break; }
        if (all_zero) break;

        char name[101];
        std::memcpy(name, hdr, 100);
        name[100] = '\0';

        // ustar prefix (offset 345, 155 bytes) for long names.
        char prefix[156];
        std::memcpy(prefix, hdr + 345, 155);
        prefix[155] = '\0';

        // size: offset 124, 12 bytes, octal.
        char szbuf[13];
        std::memcpy(szbuf, hdr + 124, 12);
        szbuf[12] = '\0';
        size_t fsize = 0;
        for (int i = 0; i < 12 && szbuf[i]; ++i) {
            if (szbuf[i] >= '0' && szbuf[i] <= '7')
                fsize = fsize * 8 + (szbuf[i] - '0');
        }

        char typeflag = static_cast<char>(hdr[156]);

        std::string full;
        if (prefix[0]) full = std::string(prefix) + "/" + std::string(name);
        else full = std::string(name);
        full = strip_dot_slash(full);

        pos += 512;
        if (typeflag == '0' || typeflag == '\0') {
            if (!full.empty() && pos + fsize <= len) {
                PayloadFile pf;
                pf.name = full;
                pf.data.assign(reinterpret_cast<const char*>(data + pos),
                               fsize);
                files.push_back(std::move(pf));
            }
        }
        // advance past data, padded to 512.
        pos += (fsize + 511) / 512 * 512;
    }
    return files;
}

const std::vector<PayloadFile>& payload_files() { return parse_payload(); }

}  // namespace forb
