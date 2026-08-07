#include <forest.h>
#include <getopt.h>

#define MBR_SIZE 512
#define MBR_PARTITION_OFFSET 446
#define MBR_SIGNATURE_OFFSET 510
#define MBR_SIGNATURE 0xAA55
#define NUM_PRIMARY_PARTITIONS 4
#define SECTOR_SIZE 512

typedef struct {
    uint8_t  boot;
    uint8_t  start_chs[3];
    uint8_t  type;
    uint8_t  end_chs[3];
    uint32_t start_lba;
    uint32_t num_sectors;
} __attribute__((packed)) PartitionEntry;

typedef struct {
    uint8_t  boot_code[MBR_PARTITION_OFFSET];
    PartitionEntry partitions[NUM_PRIMARY_PARTITIONS];
    uint16_t signature;
} __attribute__((packed)) MBR;

static const char *partition_type_name(uint8_t type) {
    switch (type) {
        case 0x00: return "Empty";
        case 0x01: return "FAT12";
        case 0x04: return "FAT16 <32M";
        case 0x05: return "Extended";
        case 0x06: return "FAT16 >32M";
        case 0x07: return "NTFS";
        case 0x0B: return "FAT32 CHS";
        case 0x0C: return "FAT32 LBA";
        case 0x0E: return "FAT16 LBA";
        case 0x0F: return "Extended LBA";
        case 0x11: return "Hidden FAT12";
        case 0x14: return "Hidden FAT16 <32M";
        case 0x16: return "Hidden FAT16 >32M";
        case 0x17: return "Hidden NTFS";
        case 0x1B: return "Hidden FAT32 CHS";
        case 0x1C: return "Hidden FAT32 LBA";
        case 0x1E: return "Hidden FAT16 LBA";
        case 0x39: return "Plan 9";
        case 0x3C: return "PartitionMagic";
        case 0x41: return "PPC PReP Boot";
        case 0x63: return "GNU Hurd";
        case 0x64: return "Novell NetWare";
        case 0x65: return "Novell NetWare";
        case 0x80: return "Minix";
        case 0x82: return "Linux swap";
        case 0x83: return "Linux";
        case 0x84: return "OS/2 hidden C:";
        case 0x85: return "Linux extended";
        case 0x86: return "NTFS volume set";
        case 0x87: return "NTFS volume set";
        case 0x9F: return "BSD/OS";
        case 0xA0: return "Solaris x86";
        case 0xA5: return "FreeBSD";
        case 0xA6: return "OpenBSD";
        case 0xA7: return "NeXTSTEP";
        case 0xB7: return "BSDI fs";
        case 0xB8: return "BSDI swap";
        case 0xC1: return "DRDOS/sec (FAT-12)";
        case 0xC4: return "DRDOS/sec (FAT-16)";
        case 0xC6: return "DRDOS/sec (FAT-16)";
        case 0xC7: return "Syrinx";
        case 0xDA: return "Non-FS data";
        case 0xDB: return "CP/M / CTOS";
        case 0xDE: return "Dell Utility";
        case 0xDF: return "BootIt";
        case 0xE1: return "DOS access";
        case 0xE3: return "DOS R/O";
        case 0xE4: return "SpeedStor";
        case 0xEB: return "BeOS fs";
        case 0xEE: return "EFI GPT";
        case 0xEF: return "EFI System";
        case 0xF0: return "Linux/PA-RISC boot";
        case 0xF1: return "SpeedStor";
        case 0xF4: return "SpeedStor";
        case 0xF2: return "DOS secondary";
        case 0xFD: return "Linux RAID autodetect";
        default:   return "Unknown";
    }
}


static void format_size(uint64_t sectors, char *buf, size_t buflen) {
    uint64_t bytes = (uint64_t)sectors * SECTOR_SIZE;
    const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double size = (double)bytes;
    int i = 0;
    while (size >= 1024.0 && i < 5) {
        size /= 1024.0;
        i++;
    }
    if (i == 0)
        snprintf(buf, buflen, "%llu B", (unsigned long long)bytes);
    else if (size >= 10.0)
        snprintf(buf, buflen, "%.0f %s", size, units[i]);
    else
        snprintf(buf, buflen, "%.1f %s", size, units[i]);
}

static void print_partition_table(const char *device, const MBR *mbr, int parseable) {
    char size_buf[32];

    if (!parseable) {
        printf("Device     Boot   Start      End  Sectors  Size    Type\n");
    }

    for (int i = 0; i < NUM_PRIMARY_PARTITIONS; i++) {
        const PartitionEntry *p = &mbr->partitions[i];

        if (p->type == 0x00)
            continue;

        char dev_name[64];
        snprintf(dev_name, sizeof(dev_name), "%s%d", device, i + 1);

        format_size(p->num_sectors, size_buf, sizeof(size_buf));

        if (parseable) {
            printf("%s,%u,%u,%u,%u,%s\n",
                   dev_name,
                   p->boot == 0x80 ? 1 : 0,
                   p->start_lba,
                   p->start_lba + p->num_sectors - 1,
                   p->num_sectors,
                   partition_type_name(p->type));
        } else {
            printf("%-10s %c   %10u %10u %10u %-7s %s\n",
                   dev_name,
                   p->boot == 0x80 ? '*' : ' ',
                   p->start_lba,
                   p->start_lba + p->num_sectors - 1,
                   p->num_sectors,
                   size_buf,
                   partition_type_name(p->type));
        }
    }
}

static int read_mbr(const char *device, MBR *mbr) {
    int fd = open(device, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "fdisk: cannot open '%s': %s\n", device, strerror(errno));
        return -1;
    }

    ssize_t n = read(fd, mbr, MBR_SIZE);
    if (n < MBR_SIZE) {
        fprintf(stderr, "fdisk: cannot read '%s': %s\n", device,
                n < 0 ? strerror(errno) : "short read");
        close(fd);
        return -1;
    }

    close(fd);

    if (mbr->signature != MBR_SIGNATURE) {
        fprintf(stderr, "fdisk: %s: no valid MBR signature (found 0x%04X, expected 0x%05X)\n",
                device, mbr->signature, MBR_SIGNATURE);
        return -1;
    }

    return 0;
}

static void list_partitions(const char *device, int parseable) {
    MBR mbr;

    if (read_mbr(device, &mbr) < 0)
        return;

    if (!parseable) {
        printf("Disk %s: %llu sectors, %s\n",
               device,
               (unsigned long long)mbr.partitions[0].num_sectors +
               (unsigned long long)mbr.partitions[1].num_sectors +
               (unsigned long long)mbr.partitions[2].num_sectors +
               (unsigned long long)mbr.partitions[3].num_sectors,
               "unknown size");
    }

    print_partition_table(device, &mbr, parseable);
}

static void partition_size(const char *device, int part_num) {
    if (part_num < 1 || part_num > NUM_PRIMARY_PARTITIONS) {
        fprintf(stderr, "fdisk: partition number must be 1-%d\n", NUM_PRIMARY_PARTITIONS);
        return;
    }

    MBR mbr;
    if (read_mbr(device, &mbr) < 0)
        return;

    const PartitionEntry *p = &mbr.partitions[part_num - 1];
    if (p->type == 0x00) {
        fprintf(stderr, "fdisk: %s%d: no such partition\n", device, part_num);
        return;
    }

    printf("%u\n", p->num_sectors);
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s [-lpsv] [-s partition] device\n", prog);
    fprintf(stderr, "  -l              list partitions\n");
    fprintf(stderr, "  -p              print in parseable format\n");
    fprintf(stderr, "  -s PARTITION    print size of partition in blocks\n");
    fprintf(stderr, "  -v              print version\n");
}

int main(int argc, char *argv[]) {
    int opt;
    int list_mode = 0;
    int parseable = 0;
    int size_mode = 0;
    int size_partition = 0;

    while ((opt = getopt(argc, argv, "lps:v")) != -1) {
        switch (opt) {
            case 'l':
                list_mode = 1;
                break;
            case 'p':
                parseable = 1;
                list_mode = 1;
                break;
            case 's':
                size_mode = 1;
                size_partition = atoi(optarg);
                break;
            case 'v':
                printf("fdisk from Forest OS\n");
                return 0;
            default:
                usage(argv[0]);
                return 1;
        }
    }

    if (optind >= argc) {
        usage(argv[0]);
        return 1;
    }

    const char *device = argv[optind];

    if (size_mode) {
        partition_size(device, size_partition);
    } else if (list_mode) {
        list_partitions(device, parseable);
    } else {
        list_partitions(device, 0);
    }

    return 0;
}
