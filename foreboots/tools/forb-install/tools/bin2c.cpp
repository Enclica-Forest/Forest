// bin2c - host build tool: turn a binary file into a C byte array.
// usage: bin2c INPUT OUTPUT SYMBOL
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s INPUT OUTPUT SYMBOL\n", argv[0]);
        return 2;
    }
    const char* in = argv[1];
    const char* out = argv[2];
    const char* sym = argv[3];

    FILE* fi = std::fopen(in, "rb");
    if (!fi) { std::perror("open input"); return 1; }
    std::fseek(fi, 0, SEEK_END);
    long n = std::ftell(fi);
    std::fseek(fi, 0, SEEK_SET);
    if (n < 0) { std::fclose(fi); return 1; }
    std::vector<unsigned char> buf(static_cast<size_t>(n));
    if (n > 0 && std::fread(buf.data(), 1, buf.size(), fi) != buf.size()) {
        std::fclose(fi);
        std::fprintf(stderr, "short read\n");
        return 1;
    }
    std::fclose(fi);

    FILE* fo = std::fopen(out, "wb");
    if (!fo) { std::perror("open output"); return 1; }
    std::fprintf(fo, "const unsigned char %s[] = {\n", sym);
    std::string line;
    char tmp[8];
    for (size_t i = 0; i < buf.size(); ++i) {
        std::snprintf(tmp, sizeof tmp, "%u,", buf[i]);
        line += tmp;
        if ((i & 31u) == 31u) { line += '\n'; std::fwrite(line.data(), 1,
                                                          line.size(), fo);
                                line.clear(); }
    }
    if (!line.empty()) std::fwrite(line.data(), 1, line.size(), fo);
    std::fprintf(fo, "\n};\nconst unsigned long %s_len = %luUL;\n", sym,
                 static_cast<unsigned long>(buf.size()));
    std::fclose(fo);
    return 0;
}
