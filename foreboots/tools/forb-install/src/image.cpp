// image.cpp - minimal PNG decoder + 24-bit BMP encoder (matches Python).
#include "forb/forb.hpp"

#include <cstdint>
#include <cstring>
#include <zlib.h>

namespace forb {

static const unsigned char PNG_SIG[8] = {0x89, 'P', 'N', 'G', '\r', '\n',
                                         0x1a, '\n'};

int paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

static uint32_t be32(const unsigned char* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

Image png_decode(const std::string& sdata) {
    const unsigned char* data =
        reinterpret_cast<const unsigned char*>(sdata.data());
    size_t len = sdata.size();
    if (len < 8 || std::memcmp(data, PNG_SIG, 8) != 0)
        throw PngError{"not a PNG file (bad signature)"};
    size_t pos = 8;
    bool have_ihdr = false;
    uint32_t w = 0, h = 0;
    int bitdepth = 0, colortype = 0, comp = 0, filt = 0, interlace = 0;
    std::string idat;
    while (pos + 8 <= len) {
        uint32_t length = be32(data + pos);
        const unsigned char* ctype = data + pos + 4;
        if (pos + 8 + length > len)
            throw PngError{"truncated chunk"};
        const unsigned char* chunk = data + pos + 8;
        pos += 12 + length;
        if (std::memcmp(ctype, "IHDR", 4) == 0) {
            if (length < 13) throw PngError{"bad IHDR"};
            w = be32(chunk);
            h = be32(chunk + 4);
            bitdepth = chunk[8];
            colortype = chunk[9];
            comp = chunk[10];
            filt = chunk[11];
            interlace = chunk[12];
            have_ihdr = true;
        } else if (std::memcmp(ctype, "IDAT", 4) == 0) {
            idat.append(reinterpret_cast<const char*>(chunk), length);
        } else if (std::memcmp(ctype, "IEND", 4) == 0) {
            break;
        }
    }
    if (!have_ihdr) throw PngError{"no IHDR chunk"};
    if (bitdepth != 8)
        throw PngError{"unsupported bit depth (need 8)"};
    if (colortype != 0 && colortype != 2 && colortype != 6)
        throw PngError{"unsupported color type (need 0/2/6)"};
    if (comp != 0 || filt != 0)
        throw PngError{"unsupported compression/filter method"};
    if (interlace != 0)
        throw PngError{"interlaced PNGs are unsupported"};
    int channels = colortype == 0 ? 1 : colortype == 2 ? 3 : 4;
    size_t stride = size_t(w) * channels;

    // zlib inflate
    std::string raw;
    {
        z_stream zs;
        std::memset(&zs, 0, sizeof zs);
        if (inflateInit(&zs) != Z_OK)
            throw PngError{"zlib init failed"};
        zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(idat.data()));
        zs.avail_in = static_cast<uInt>(idat.size());
        char buf[65536];
        int ret;
        do {
            zs.next_out = reinterpret_cast<Bytef*>(buf);
            zs.avail_out = sizeof buf;
            ret = inflate(&zs, Z_NO_FLUSH);
            if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
                inflateEnd(&zs);
                throw PngError{"zlib decompress failed"};
            }
            raw.append(buf, sizeof(buf) - zs.avail_out);
            if (ret == Z_BUF_ERROR && zs.avail_in == 0) break;
        } while (ret != Z_STREAM_END);
        inflateEnd(&zs);
    }
    if (raw.size() < (stride + 1) * h)
        throw PngError{"truncated image data"};

    unsigned char* rp = reinterpret_cast<unsigned char*>(
        const_cast<char*>(raw.data()));
    std::vector<std::string> rows;
    std::string prev(stride, '\0');
    size_t off = 0;
    int bpp = channels;
    for (uint32_t y = 0; y < h; ++y) {
        int ftype = rp[off++];
        std::string line(reinterpret_cast<char*>(rp + off), stride);
        off += stride;
        unsigned char* l = reinterpret_cast<unsigned char*>(&line[0]);
        const unsigned char* pv =
            reinterpret_cast<const unsigned char*>(prev.data());
        if (ftype == 0) {
            // none
        } else if (ftype == 1) {
            for (size_t i = bpp; i < stride; ++i)
                l[i] = (l[i] + l[i - bpp]) & 0xFF;
        } else if (ftype == 2) {
            for (size_t i = 0; i < stride; ++i)
                l[i] = (l[i] + pv[i]) & 0xFF;
        } else if (ftype == 3) {
            for (size_t i = 0; i < stride; ++i) {
                int left = i >= size_t(bpp) ? l[i - bpp] : 0;
                l[i] = (l[i] + ((left + pv[i]) >> 1)) & 0xFF;
            }
        } else if (ftype == 4) {
            for (size_t i = 0; i < stride; ++i) {
                int a = i >= size_t(bpp) ? l[i - bpp] : 0;
                int b = pv[i];
                int c = i >= size_t(bpp) ? pv[i - bpp] : 0;
                l[i] = (l[i] + paeth(a, b, c)) & 0xFF;
            }
        } else {
            throw PngError{"bad filter type"};
        }
        rows.push_back(line);
        prev = line;
    }

    Image img;
    img.w = static_cast<int>(w);
    img.h = static_cast<int>(h);
    for (const std::string& line : rows) {
        if (colortype == 2) {
            img.rows.push_back(line);
        } else if (colortype == 6) {
            std::string rgb(size_t(w) * 3, '\0');
            for (uint32_t x = 0; x < w; ++x) {
                rgb[x * 3 + 0] = line[x * 4 + 0];
                rgb[x * 3 + 1] = line[x * 4 + 1];
                rgb[x * 3 + 2] = line[x * 4 + 2];
            }
            img.rows.push_back(rgb);
        } else {
            std::string rgb(size_t(w) * 3, '\0');
            for (uint32_t x = 0; x < w; ++x) {
                rgb[x * 3 + 0] = line[x];
                rgb[x * 3 + 1] = line[x];
                rgb[x * 3 + 2] = line[x];
            }
            img.rows.push_back(rgb);
        }
    }
    return img;
}

static void put_le32(std::string& o, uint32_t v) {
    o.push_back(char(v & 0xFF));
    o.push_back(char((v >> 8) & 0xFF));
    o.push_back(char((v >> 16) & 0xFF));
    o.push_back(char((v >> 24) & 0xFF));
}
static void put_le16(std::string& o, uint16_t v) {
    o.push_back(char(v & 0xFF));
    o.push_back(char((v >> 8) & 0xFF));
}

std::string bmp_encode(int w, int h, const std::vector<std::string>& rows) {
    int row_bytes = w * 3;
    int pad = (4 - (row_bytes % 4)) % 4;
    std::string padding(pad, '\0');
    uint32_t image_size = uint32_t(row_bytes + pad) * h;
    uint32_t file_size = 14 + 40 + image_size;
    std::string out;
    out += "BM";
    put_le32(out, file_size);
    put_le16(out, 0);
    put_le16(out, 0);
    put_le32(out, 14 + 40);
    // BITMAPINFOHEADER
    put_le32(out, 40);
    put_le32(out, static_cast<uint32_t>(w));
    put_le32(out, static_cast<uint32_t>(h));
    put_le16(out, 1);
    put_le16(out, 24);
    put_le32(out, 0);
    put_le32(out, image_size);
    put_le32(out, 2835);
    put_le32(out, 2835);
    put_le32(out, 0);
    put_le32(out, 0);
    for (int y = h - 1; y >= 0; --y) {
        const std::string& row = rows[y];
        std::string bgr(row_bytes, '\0');
        for (int x = 0; x < w; ++x) {
            bgr[x * 3 + 0] = row[x * 3 + 2];
            bgr[x * 3 + 1] = row[x * 3 + 1];
            bgr[x * 3 + 2] = row[x * 3 + 0];
        }
        out += bgr;
        out += padding;
    }
    return out;
}

std::string png_to_bmp(const std::string& data) {
    Image img = png_decode(data);
    return bmp_encode(img.w, img.h, img.rows);
}

}  // namespace forb
