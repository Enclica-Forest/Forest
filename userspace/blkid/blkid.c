#define _POSIX_C_SOURCE 200809L
#include <forest.h>
#include <getopt.h>

#define BLKID_READ_SIZE 4096

enum output_format {
    FMT_FULL,
    FMT_VALUE,
    FMT_DEVICE,
    FMT_EXPORT
};

typedef struct {
    char devname[256];
    char type[32];
    char uuid[64];
    char label[256];
} BlockDeviceInfo;

static int output_format = FMT_FULL;
static int low_level = 0;
static char *search_tag = NULL;
static char *show_tag = NULL;

static int read_block(int fd, unsigned char *buf, size_t size, off_t offset) {
    ssize_t n;
    n = pread(fd, buf, size, offset);
    return (n == (ssize_t)size) ? 0 : -1;
}

static void to_upper_uuid(unsigned char *uuid, char *out, size_t len) {
    snprintf(out, len,
        "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
        uuid[0], uuid[1], uuid[2], uuid[3],
        uuid[4], uuid[5], uuid[6], uuid[7],
        uuid[8], uuid[9], uuid[10], uuid[11],
        uuid[12], uuid[13], uuid[14], uuid[15]);
}

static int detect_ext(int fd, BlockDeviceInfo *info) {
    unsigned char buf[4096];
    if (read_block(fd, buf, 4096, 1024) < 0)
        return -1;

    uint16_t magic = buf[0x38] | (buf[0x39] << 8);
    if (magic != 0xEF53)
        return -1;

    uint32_t features = buf[0x60] | (buf[0x61] << 8) |
                        (buf[0x62] << 16) | (buf[0x63] << 24);

    if (features & 0x40)
        strcpy(info->type, "ext4");
    else if (features & 0x20)
        strcpy(info->type, "ext3");
    else
        strcpy(info->type, "ext2");

    uint16_t rev = buf[0x0] | (buf[0x1] << 8);
    unsigned int uuid_off = (rev >= 1) ? 0x68 : 0x468;
    to_upper_uuid(buf + uuid_off, info->uuid, sizeof(info->uuid));

    int label_off = uuid_off + 16;
    memcpy(info->label, buf + label_off, 16);
    info->label[16] = '\0';

    return 0;
}

static int detect_fat(int fd, BlockDeviceInfo *info) {
    unsigned char buf[512];
    if (read_block(fd, buf, 512, 0) < 0)
        return -1;

    if (buf[0] != 0xEB && buf[0] != 0xE9)
        return -1;

    uint16_t bps = buf[11] | (buf[12] << 8);
    uint8_t spc = buf[13];
    uint16_t rsvd = buf[14] | (buf[15] << 8);
    uint8_t nfat = buf[16];
    uint16_t root_entries = buf[17] | (buf[18] << 8);
    uint16_t total_sectors16 = buf[19] | (buf[20] << 8);
    uint16_t fat_size16 = buf[22] | (buf[23] << 8);
    uint32_t total_sectors32 = buf[32] | (buf[33] << 8) |
                               (buf[34] << 16) | (buf[35] << 24);

    uint32_t fat_size = fat_size16;
    if (fat_size == 0)
        fat_size = buf[36] | (buf[37] << 8) |
                  (buf[38] << 16) | (buf[39] << 24);

    uint32_t total_sectors = total_sectors16 ? total_sectors16 : total_sectors32;
    uint32_t data_sectors = total_sectors - rsvd - (nfat * fat_size) -
                            (root_entries * 32 + bps - 1) / bps;
    uint32_t total_clusters = data_sectors / spc;

    if (total_clusters < 4085)
        strcpy(info->type, "vfat"); /* FAT12 */
    else if (total_clusters < 65525)
        strcpy(info->type, "vfat"); /* FAT16 */
    else
        strcpy(info->type, "vfat"); /* FAT32 */

    unsigned int ext_boot_sig = buf[66];
    if (ext_boot_sig == 0x29) {
        if (total_clusters >= 65525) {
            to_upper_uuid(buf + 73, info->uuid, sizeof(info->uuid));
            memcpy(info->label, buf + 43, 11);
        } else {
            to_upper_uuid(buf + 39, info->uuid, sizeof(info->uuid));
            memcpy(info->label, buf + 43, 11);
        }
        info->label[11] = '\0';
    }

    return 0;
}

static int detect_iso9660(int fd, BlockDeviceInfo *info) {
    unsigned char buf[512];

    if (read_block(fd, buf, 512, 0x8001) < 0)
        return -1;

    if (memcmp(buf, "CD001", 5) != 0)
        return -1;

    strcpy(info->type, "iso9660");

    if (read_block(fd, buf, 512, 0x8001) < 0)
        return -1;

    if (buf[88] == 0x01 && buf[89] == 0x43 && buf[90] == 0x44) {
        memcpy(info->label, buf + 40, 32);
        info->label[32] = '\0';
    }

    info->uuid[0] = '\0';
    return 0;
}

static int detect_ntfs(int fd, BlockDeviceInfo *info) {
    unsigned char buf[512];
    if (read_block(fd, buf, 512, 0) < 0)
        return -1;

    if (memcmp(buf + 3, "NTFS    ", 8) != 0)
        return -1;

    strcpy(info->type, "ntfs");

    uint32_t bps = buf[11] | (buf[12] << 8);
    uint16_t spc = buf[13];

    uint64_t mft_cluster = buf[48] | (buf[49] << 8) |
                           (buf[50] << 16) | (buf[51] << 24) |
                           ((uint64_t)buf[52] << 32) |
                           ((uint64_t)buf[53] << 40) |
                           ((uint64_t)buf[54] << 48) |
                           ((uint64_t)buf[55] << 56);

    uint64_t mft_offset = mft_cluster * spc * bps;
    unsigned char mft_buf[1024];
    if (pread(fd, mft_buf, 1024, mft_offset) != 1024)
        return -1;

    if (memcmp(mft_buf, "FILE", 4) != 0)
        return -1;

    uint16_t attr_offset = mft_buf[20] | (mft_buf[21] << 8);

    uint32_t attr_type = mft_buf[attr_offset] |
                         (mft_buf[attr_offset + 1] << 8) |
                         (mft_buf[attr_offset + 2] << 16) |
                         (mft_buf[attr_offset + 3] << 24);

    if (attr_type == 0x30) {
        uint32_t name_offset = mft_buf[attr_offset + 20] |
                               (mft_buf[attr_offset + 21] << 8);
        uint32_t name_len = mft_buf[attr_offset + 64];
        if (name_len > 0 && name_len < 128) {
            memcpy(info->label, mft_buf + attr_offset + name_offset,
                   name_len * 2);
            info->label[name_len] = '\0';
        }
    }

    if (attr_offset + 24 < 1024) {
        uint32_t next_type = mft_buf[attr_offset + 24] |
                             (mft_buf[attr_offset + 25] << 8) |
                             (mft_buf[attr_offset + 26] << 16) |
                             (mft_buf[attr_offset + 27] << 24);
        if (next_type == 0x60) {
            uint32_t vol_offset = attr_offset + 24;
            uint32_t vol_attr_len = mft_buf[vol_offset + 4] |
                                    (mft_buf[vol_offset + 5] << 8);
            (void)vol_attr_len;
        }
    }

    return 0;
}

static int detect_swap(int fd, BlockDeviceInfo *info) {
    unsigned char buf[512];

    if (read_block(fd, buf, 512, 4086) < 0)
        return -1;

    if (memcmp(buf, "SWAPSPACE2", 10) == 0) {
        strcpy(info->type, "swap");
        info->uuid[0] = '\0';
        info->label[0] = '\0';
        return 0;
    }

    if (memcmp(buf, "SWAP1", 5) == 0) {
        strcpy(info->type, "swap");
        info->uuid[0] = '\0';
        info->label[0] = '\0';
        return 0;
    }

    return -1;
}

static int probe_device(const char *devpath, BlockDeviceInfo *info) {
    int fd;
    int found = 0;

    memset(info, 0, sizeof(*info));
    strncpy(info->devname, devpath, sizeof(info->devname) - 1);

    fd = open(devpath, O_RDONLY);
    if (fd < 0)
        return -1;

    if (detect_ext(fd, info) == 0) {
        found = 1;
    } else if (detect_fat(fd, info) == 0) {
        found = 1;
    } else if (detect_ntfs(fd, info) == 0) {
        found = 1;
    } else if (detect_iso9660(fd, info) == 0) {
        found = 1;
    } else if (detect_swap(fd, info) == 0) {
        found = 1;
    }

    close(fd);
    return found ? 0 : -1;
}

static void print_device_full(const BlockDeviceInfo *info) {
    printf("%s: TYPE=\"%s\"", info->devname, info->type);
    if (info->uuid[0])
        printf(" UUID=\"%s\"", info->uuid);
    if (info->label[0])
        printf(" LABEL=\"%s\"", info->label);
    printf("\n");
}

static void print_device_value(const BlockDeviceInfo *info) {
    if (show_tag) {
        if (strcmp(show_tag, "TYPE") == 0)
            printf("%s\n", info->type);
        else if (strcmp(show_tag, "UUID") == 0)
            printf("%s\n", info->uuid);
        else if (strcmp(show_tag, "LABEL") == 0)
            printf("%s\n", info->label);
    } else {
        printf("%s\n", info->type);
    }
}

static void print_device_device(const BlockDeviceInfo *info) {
    printf("%s\n", info->devname);
}

static void print_device_export(const BlockDeviceInfo *info) {
    printf("DEVNAME=%s\n", info->devname);
    printf("TYPE=%s\n", info->type);
    if (info->uuid[0])
        printf("UUID=%s\n", info->uuid);
    if (info->label[0])
        printf("LABEL=%s\n", info->label);
}

static void print_device(const BlockDeviceInfo *info) {
    switch (output_format) {
        case FMT_FULL:    print_device_full(info);    break;
        case FMT_VALUE:   print_device_value(info);   break;
        case FMT_DEVICE:  print_device_device(info);  break;
        case FMT_EXPORT:  print_device_export(info);  break;
    }
}

static int is_block_device(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0)
        return 0;
    return S_ISBLK(st.st_mode);
}

static int probe_and_print(const char *devpath) {
    BlockDeviceInfo info;
    if (probe_device(devpath, &info) < 0)
        return 1;

    if (search_tag) {
        int match = 0;
        if (strcmp(search_tag, "TYPE") == 0 && info.type[0])
            match = 1;
        else if (strcmp(search_tag, "UUID") == 0 && info.uuid[0])
            match = 1;
        else if (strcmp(search_tag, "LABEL") == 0 && info.label[0])
            match = 1;
        if (!match)
            return 1;
    }

    print_device(&info);
    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [-o FORMAT] [-p] [-s TAG] [-t TAG] [device ...]\n", prog);
    fprintf(stderr, "  -o FORMAT  Output format: full, value, device, export\n");
    fprintf(stderr, "  -p         Low-level probing\n");
    fprintf(stderr, "  -s TAG     Show specific tag (TYPE, UUID, LABEL)\n");
    fprintf(stderr, "  -t TAG     Search for devices with specified tag\n");
}

int main(int argc, char *argv[]) {
    int opt;
    int exit_code = 0;
    int has_args = 0;

    while ((opt = getopt(argc, argv, "o:pst:h")) != -1) {
        switch (opt) {
            case 'o':
                if (strcmp(optarg, "full") == 0)
                    output_format = FMT_FULL;
                else if (strcmp(optarg, "value") == 0)
                    output_format = FMT_VALUE;
                else if (strcmp(optarg, "device") == 0)
                    output_format = FMT_DEVICE;
                else if (strcmp(optarg, "export") == 0)
                    output_format = FMT_EXPORT;
                else {
                    fprintf(stderr, "Unknown output format: %s\n", optarg);
                    return 1;
                }
                break;
            case 'p':
                low_level = 1;
                break;
            case 's':
                if (optind < argc && argv[optind][0] != '-') {
                    show_tag = argv[optind];
                    optind++;
                }
                break;
            case 't':
                if (optind < argc && argv[optind][0] != '-') {
                    search_tag = argv[optind];
                    optind++;
                }
                break;
            case 'h':
            default:
                usage(argv[0]);
                return (opt == 'h') ? 0 : 1;
        }
    }

    if (optind < argc)
        has_args = 1;

    if (!has_args) {
        const char *devices[] = {
            "/dev/sda", "/dev/sdb", "/dev/sdc", "/dev/sdd",
            "/dev/hda", "/dev/hdb",
            "/dev/vda", "/dev/vdb", "/dev/vdc",
            NULL
        };

        for (int i = 0; devices[i]; i++) {
            if (is_block_device(devices[i])) {
                if (probe_and_print(devices[i]) == 0)
                    exit_code = 0;
            }
        }
    } else {
        for (int i = optind; i < argc; i++) {
            if (probe_and_print(argv[i]) < 0)
                exit_code = 1;
        }
    }

    return exit_code;
}
