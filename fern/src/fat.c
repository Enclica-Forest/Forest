#include "include/fat.h"
#include "include/fs_internal.h"
#include "include/enhanced_heap.h"
#include "include/debuglog.h"
#include "include/string.h"
#include "include/memory.h"
#include "include/vfs.h"

#define FAT_BOOT_SIGNATURE 0xAA55

typedef struct {
    uint8_t name[11];
    uint8_t attr;
    uint8_t reserved;
    uint8_t create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t high_cluster;
    uint16_t write_time;
    uint16_t write_date;
    uint16_t low_cluster;
    uint32_t size;
} __attribute__((packed)) fat_dir_entry_t;

#define FAT_ATTR_READ_ONLY   0x01
#define FAT_ATTR_HIDDEN      0x02
#define FAT_ATTR_SYSTEM      0x04
#define FAT_ATTR_VOLUME_ID   0x08
#define FAT_ATTR_DIRECTORY   0x10
#define FAT_ATTR_ARCHIVE     0x20
#define FAT_ATTR_LFN         0x0F

#define FAT_ENTRY_DELETED    0xE5
#define FAT_ENTRY_END        0x00

typedef struct {
    uint8_t order;
    uint8_t name1[10];
    uint8_t attr;
    uint8_t type;
    uint8_t checksum;
    uint8_t name2[12];
    uint16_t cluster_low;
    uint8_t name3[4];
} __attribute__((packed)) fat_lfn_entry_t;

static fat_fs_t* g_fat_fs = NULL;

static uint32_t fat_read_sector(fat_fs_t* fs, uint64_t lba, uint8_t* buffer) {
    if (!fs || !fs->read_sector) return 0;
    return fs->read_sector(fs->dev_data, lba, buffer);
}

static uint32_t fat_write_sector(fat_fs_t* fs, uint64_t lba, uint8_t* buffer) {
    if (!fs || !fs->write_sector) return 0;
    fs->dirty = true;
    return fs->write_sector(fs->dev_data, lba, buffer);
}

static uint32_t fat_get_next_cluster(fat_fs_t* fs, uint32_t cluster) {
    if (cluster >= fs->total_clusters) return 0;
    
    uint32_t fat_offset;
    if (fs->fat_type == FAT12) {
        fat_offset = cluster + (cluster / 2);
    } else if (fs->fat_type == FAT16) {
        fat_offset = cluster * 2;
    } else {
        fat_offset = cluster * 4;
    }
    
    uint32_t sector = fs->fat_start + (fat_offset / fs->bytes_per_sector);
    uint32_t offset = fat_offset % fs->bytes_per_sector;
    
    uint8_t buffer[512];
    if (fat_read_sector(fs, sector, buffer) != 512) return 0;
    
    uint32_t value;
    if (fs->fat_type == FAT12) {
        uint16_t v = *(uint16_t*)&buffer[offset];
        if (cluster & 1) {
            value = v >> 4;
        } else {
            value = v & 0x0FFF;
        }
        if (value >= 0xFF8) return 0xFFFFFFFF;
    } else if (fs->fat_type == FAT16) {
        value = *(uint16_t*)&buffer[offset];
        if (value >= 0xFFF8) return 0xFFFFFFFF;
    } else {
        value = *(uint32_t*)&buffer[offset] & 0x0FFFFFFF;
        if (value >= 0x0FFFFFF8) return 0xFFFFFFFF;
    }
    
    return value;
}

static uint32_t fat_cluster_to_lba(fat_fs_t* fs, uint32_t cluster) {
    if (cluster < 2 || cluster >= fs->total_clusters) return 0;
    return fs->data_start + ((cluster - 2) * fs->sectors_per_cluster);
}

static uint32_t fat_read_cluster_chain(fat_fs_t* fs, uint32_t start_cluster, 
                                        uint32_t offset, uint32_t size, uint8_t* buffer) {
    uint32_t cluster_size = fs->sectors_per_cluster * fs->bytes_per_sector;
    uint32_t cluster = start_cluster;
    uint32_t bytes_read = 0;
    uint32_t skip_clusters = offset / cluster_size;
    uint32_t skip_bytes = offset % cluster_size;
    
    for (uint32_t i = 0; i < skip_clusters && cluster < 0xFFFFFFFF; i++) {
        cluster = fat_get_next_cluster(fs, cluster);
        if (cluster == 0xFFFFFFFF || cluster == 0) break;
    }
    
    while (cluster < 0xFFFFFFFF && cluster != 0 && bytes_read < size) {
        uint32_t sector = fat_cluster_to_lba(fs, cluster);
        uint32_t to_read = cluster_size - skip_bytes;
        if (to_read > size - bytes_read) {
            to_read = size - bytes_read;
        }
        
        for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
            uint32_t read = fat_read_sector(fs, sector + s, buffer + bytes_read + s * fs->bytes_per_sector);
            if (read != fs->bytes_per_sector) break;
        }
        
        bytes_read += to_read;
        
        if (bytes_read >= size) break;
        
        cluster = fat_get_next_cluster(fs, cluster);
        skip_bytes = 0;
    }
    
    return bytes_read;
}

static void fat_read_dir_entry(fat_fs_t* fs, uint32_t cluster, uint32_t index, fat_dir_entry_t* entry) {
    uint32_t cluster_size = fs->sectors_per_cluster * fs->bytes_per_sector;
    uint32_t offset = index * sizeof(fat_dir_entry_t);
    uint32_t skip_clusters = offset / cluster_size;
    uint32_t skip_bytes = offset % cluster_size;
    
    uint8_t* buffer = (uint8_t*)enhanced_heap_alloc(cluster_size, "fat_dir_buffer");
    if (!buffer) return;
    
    uint32_t c = cluster;
    for (uint32_t i = 0; i < skip_clusters && c < 0xFFFFFFFF; i++) {
        c = fat_get_next_cluster(fs, c);
    }
    
    if (c < 0xFFFFFFFF && c != 0) {
        fat_read_cluster_chain(fs, c, skip_bytes, sizeof(fat_dir_entry_t), buffer);
        memcpy(entry, buffer, sizeof(fat_dir_entry_t));
    } else {
        memset(entry, 0, sizeof(fat_dir_entry_t));
    }
    
    enhanced_heap_free(buffer, "fat_dir_buffer");
}

static bool fat_find_file_in_dir(fat_fs_t* fs, uint32_t dir_cluster, 
                                  const char* name, fat_dir_entry_t* entry, uint32_t* entry_index) {
    uint32_t cluster_size = fs->sectors_per_cluster * fs->bytes_per_sector;
    uint32_t entries_per_cluster = cluster_size / sizeof(fat_dir_entry_t);
    uint32_t index = 0;
    uint32_t cluster = dir_cluster;
    
    char short_name[12];
    memset(short_name, ' ', 11);
    short_name[11] = 0;
    
    const char* basename = name;
    const char* ext = NULL;
    for (int i = strlen(name) - 1; i >= 0; i--) {
        if (name[i] == '.') {
            ext = name + i + 1;
            break;
        }
    }
    
    if (ext) {
        int baselen = ext - name;
        if (baselen > 8) baselen = 8;
        memcpy(short_name, name, baselen);
        if (strlen(ext) > 3) {
            memcpy(short_name + 8, ext, 3);
        } else {
            memcpy(short_name + 8, ext, strlen(ext));
        }
    } else {
        int len = strlen(name);
        if (len > 8) len = 8;
        memcpy(short_name, name, len);
    }
    
    for (uint8_t c = 0; c < 11; c++) {
        if (short_name[c] >= 'a' && short_name[c] <= 'z') {
            short_name[c] -= 32;
        }
    }
    
    uint8_t* buffer = (uint8_t*)enhanced_heap_alloc(cluster_size, "fat_dir_find");
    if (!buffer) return false;
    
    while (cluster < 0xFFFFFFFF && cluster != 0) {
        uint32_t sector = fat_cluster_to_lba(fs, cluster);
        for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
            fat_read_sector(fs, sector + s, buffer + s * fs->bytes_per_sector);
        }
        
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            fat_dir_entry_t* e = (fat_dir_entry_t*)&buffer[i * sizeof(fat_dir_entry_t)];
            
            if (e->name[0] == FAT_ENTRY_END) {
                enhanced_heap_free(buffer, "fat_dir_find");
                return false;
            }
            
            if (e->name[0] == FAT_ENTRY_DELETED) continue;
            if (e->attr == FAT_ATTR_LFN) continue;
            
            bool match = true;
            for (int j = 0; j < 11; j++) {
                if (e->name[j] != short_name[j]) {
                    match = false;
                    break;
                }
            }
            
            if (match) {
                if (entry) memcpy(entry, e, sizeof(fat_dir_entry_t));
                if (entry_index) *entry_index = index;
                enhanced_heap_free(buffer, "fat_dir_find");
                return true;
            }
            
            index++;
        }
        
        cluster = fat_get_next_cluster(fs, cluster);
    }
    
    enhanced_heap_free(buffer, "fat_dir_find");
    return false;
}

static fat_dir_entry_t* fat_read_root_dir(fat_fs_t* fs, uint32_t* count) {
    uint32_t entries = fs->root_entries;
    uint32_t size = entries * sizeof(fat_dir_entry_t);
    uint8_t* buffer = (uint8_t*)enhanced_heap_alloc(size, "fat_root_dir");
    if (!buffer) return NULL;
    
    for (uint32_t i = 0; i < fs->root_sectors; i++) {
        fat_read_sector(fs, fs->root_start + i, buffer + i * fs->bytes_per_sector);
    }
    
    if (count) {
        *count = entries;
    }
    
    return (fat_dir_entry_t*)buffer;
}

static void fat_free_root_dir(fat_dir_entry_t* dir) {
    if (dir) {
        enhanced_heap_free(dir, "fat_root_dir");
    }
}

static uint32_t fat_alloc_cluster(fat_fs_t* fs) {
    for (uint32_t i = 2; i < fs->total_clusters; i++) {
        uint32_t fat_offset;
        if (fs->fat_type == FAT16) {
            fat_offset = i * 2;
        } else {
            fat_offset = i * 4;
        }
        
        uint32_t sector = fs->fat_start + (fat_offset / fs->bytes_per_sector);
        uint32_t offset = fat_offset % fs->bytes_per_sector;
        
        uint8_t buffer[4];
        if (fat_read_sector(fs, sector, buffer) != 512) continue;
        
        uint32_t value;
        if (fs->fat_type == FAT16) {
            value = *(uint16_t*)&buffer[offset];
            if (value == 0) {
                *(uint16_t*)&buffer[offset] = 0xFFFF;
                fat_write_sector(fs, sector, buffer);
                return i;
            }
        } else {
            value = *(uint32_t*)&buffer[offset] & 0x0FFFFFFF;
            if (value == 0) {
                *(uint32_t*)&buffer[offset] = 0x0FFFFFFF;
                fat_write_sector(fs, sector, buffer);
                return i;
            }
        }
    }
    return 0;
}

static void fat_free_cluster_chain(fat_fs_t* fs, uint32_t start_cluster) {
    uint32_t cluster = start_cluster;
    while (cluster < 0xFFFFFFFF && cluster != 0) {
        uint32_t next = fat_get_next_cluster(fs, cluster);
        
        uint32_t fat_offset;
        if (fs->fat_type == FAT16) {
            fat_offset = cluster * 2;
        } else {
            fat_offset = cluster * 4;
        }
        
        uint32_t sector = fs->fat_start + (fat_offset / fs->bytes_per_sector);
        uint32_t offset = fat_offset % fs->bytes_per_sector;
        
        uint8_t buffer[4];
        if (fat_read_sector(fs, sector, buffer) == 512) {
            if (fs->fat_type == FAT16) {
                *(uint16_t*)&buffer[offset] = 0;
            } else {
                *(uint32_t*)&buffer[offset] = 0;
            }
            fat_write_sector(fs, sector, buffer);
        }
        
        cluster = next;
    }
}

static uint32_t fat_node_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    if (!node || !buffer) return 0;
    
    fat_node_t* fat_node = (fat_node_t*)node->internal_data;
    if (!fat_node || !fat_node->fs) return 0;
    
    fat_fs_t* fs = fat_node->fs;
    
    if (node->flags & VFS_DIRECTORY) {
        return 0;
    }
    
    if (offset >= node->length) return 0;
    if (offset + size > node->length) {
        size = node->length - offset;
    }
    
    return fat_read_cluster_chain(fs, fat_node->start_cluster, offset, size, buffer);
}

static void fat_node_open(vfs_node_t* node, uint32_t flags) {
    if (!node) return;
    node->open_count++;
}

static void fat_node_close(vfs_node_t* node) {
    if (!node) return;
    
    if (node->open_count > 0) {
        node->open_count--;
    }
    
    if (node->open_count == 0 && (node->flags & VFS_DELETED)) {
        node->flags |= VFS_DELETING;
    }
}

static bool fat_node_readdir(vfs_node_t* node, uint32_t index, vfs_dirent_t* dirent) {
    if (!node || !dirent) return false;
    if (!(node->flags & VFS_DIRECTORY)) return false;
    
    fat_node_t* fat_node = (fat_node_t*)node->internal_data;
    if (!fat_node || !fat_node->fs) return false;
    
    fat_fs_t* fs = fat_node->fs;
    uint32_t cluster = fat_node->start_cluster;
    uint32_t cluster_size = fs->sectors_per_cluster * fs->bytes_per_sector;
    uint32_t entries_per_cluster = cluster_size / sizeof(fat_dir_entry_t);
    
    uint8_t* buffer = (uint8_t*)enhanced_heap_alloc(cluster_size, "fat_readdir");
    if (!buffer) return false;
    
    uint32_t current_cluster = cluster;
    uint32_t current_index = 0;
    
    while (current_cluster < 0xFFFFFFFF && current_cluster != 0) {
        uint32_t sector = fat_cluster_to_lba(fs, current_cluster);
        for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
            fat_read_sector(fs, sector + s, buffer + s * fs->bytes_per_sector);
        }
        
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            fat_dir_entry_t* entry = (fat_dir_entry_t*)&buffer[i * sizeof(fat_dir_entry_t)];
            
            if (entry->name[0] == FAT_ENTRY_END) {
                enhanced_heap_free(buffer, "fat_readdir");
                return false;
            }
            
            if (entry->name[0] == FAT_ENTRY_DELETED) continue;
            if (entry->attr == FAT_ATTR_VOLUME_ID) continue;
            
            if (current_index == index) {
                char name[256];
                int j = 0;
                while (j < 8 && entry->name[j] != ' ') {
                    name[j++] = entry->name[j];
                }
                if (entry->name[8] != ' ') {
                    name[j++] = '.';
                    int k = 8;
                    while (k < 11 && entry->name[k] != ' ') {
                        name[j++] = entry->name[k++];
                    }
                }
                name[j] = '\0';
                
                strncpy(dirent->name, name, sizeof(dirent->name) - 1);
                dirent->inode = current_index;
                
                enhanced_heap_free(buffer, "fat_readdir");
                return true;
            }
            
            current_index++;
        }
        
        current_cluster = fat_get_next_cluster(fs, current_cluster);
    }
    
    enhanced_heap_free(buffer, "fat_readdir");
    return false;
}

static vfs_node_t* fat_node_finddir(vfs_node_t* node, const char* name) {
    if (!node || !name) return NULL;
    if (!(node->flags & VFS_DIRECTORY)) return NULL;
    
    fat_node_t* fat_node = (fat_node_t*)node->internal_data;
    if (!fat_node || !fat_node->fs) return NULL;
    
    fat_fs_t* fs = fat_node->fs;
    uint32_t dir_cluster = fat_node->start_cluster;
    
    fat_dir_entry_t entry;
    if (fat_find_file_in_dir(fs, dir_cluster, name, &entry, NULL)) {
        vfs_node_t* new_node = (vfs_node_t*)enhanced_heap_alloc(sizeof(vfs_node_t), "fat_vfs_node");
        if (!new_node) return NULL;
        
        memset(new_node, 0, sizeof(vfs_node_t));
        
        char shortname[9];
        memcpy(shortname, entry.name, 8);
        shortname[8] = 0;
        
        char ext[4];
        memcpy(ext, entry.name + 8, 3);
        ext[3] = 0;
        
        if (ext[0] != ' ') {
            string_format(new_node->name, sizeof(new_node->name), "%s.%s", shortname, ext);
        } else {
            strncpy(new_node->name, shortname, sizeof(new_node->name) - 1);
        }
        
        if (entry.attr & FAT_ATTR_DIRECTORY) {
            new_node->flags = VFS_DIRECTORY;
            new_node->length = 0;
        } else {
            new_node->flags = VFS_FILE;
            new_node->length = entry.size;
        }
        
        new_node->inode = entry.low_cluster | ((uint32_t)entry.high_cluster << 16);
        
        fat_node_t* new_fat_node = (fat_node_t*)enhanced_heap_alloc(sizeof(fat_node_t), "fat_node");
        if (!new_fat_node) {
            enhanced_heap_free(new_node, "fat_vfs_node");
            return NULL;
        }
        
        memset(new_fat_node, 0, sizeof(fat_node_t));
        new_fat_node->fs = fs;
        new_fat_node->start_cluster = new_node->inode;
        
        new_node->internal_data = new_fat_node;
        new_node->read = fat_node_read;
        new_node->open = fat_node_open;
        new_node->close = fat_node_close;
        new_node->readdir = fat_node_readdir;
        new_node->finddir = fat_node_finddir;
        
        return new_node;
    }
    
    return NULL;
}

static int fat_node_unlink(vfs_node_t* node, const char* name) {
    if (!node || !name) return -1;
    
    fat_node_t* fat_node = (fat_node_t*)node->internal_data;
    if (!fat_node || !fat_node->fs) return -1;
    
    fat_fs_t* fs = fat_node->fs;
    
    fat_dir_entry_t entry;
    uint32_t entry_index;
    if (!fat_find_file_in_dir(fs, fat_node->start_cluster, name, &entry, &entry_index)) {
        return -1;
    }
    
    uint32_t cluster = entry.low_cluster | ((uint32_t)entry.high_cluster << 16);
    if (cluster > 0 && !(entry.attr & FAT_ATTR_DIRECTORY)) {
        fat_free_cluster_chain(fs, cluster);
    }
    
    uint32_t cluster_size = fs->sectors_per_cluster * fs->bytes_per_sector;
    uint32_t sector = fat_cluster_to_lba(fs, fat_node->start_cluster);
    uint32_t offset = entry_index * sizeof(fat_dir_entry_t);
    
    uint8_t* buffer = (uint8_t*)enhanced_heap_alloc(cluster_size, "fat_unlink");
    if (!buffer) return -1;
    
    uint32_t sector_num = sector + (offset / fs->bytes_per_sector);
    fat_read_sector(fs, sector_num, buffer);
    
    buffer[offset % fs->bytes_per_sector] = FAT_ENTRY_DELETED;
    
    fat_write_sector(fs, sector_num, buffer);
    enhanced_heap_free(buffer, "fat_unlink");
    
    return 0;
}

static int fat_node_mkdir(vfs_node_t* node, const char* name, uint32_t mode) {
    (void)mode;
    if (!node || !name) return -1;
    
    fat_node_t* fat_node = (fat_node_t*)node->internal_data;
    if (!fat_node || !fat_node->fs) return -1;
    
    fat_fs_t* fs = fat_node->fs;
    
    uint32_t new_cluster = fat_alloc_cluster(fs);
    if (!new_cluster) return -1;
    
    uint8_t* buffer = (uint8_t*)enhanced_heap_alloc(fs->bytes_per_sector, "fat_mkdir");
    if (!buffer) {
        fat_free_cluster_chain(fs, new_cluster);
        return -1;
    }
    
    memset(buffer, 0, fs->bytes_per_sector);
    fat_dir_entry_t* dot = (fat_dir_entry_t*)buffer;
    memset(dot->name, ' ', 11);
    dot->name[0] = '.';
    dot->attr = FAT_ATTR_DIRECTORY;
    dot->low_cluster = new_cluster & 0xFFFF;
    dot->high_cluster = (new_cluster >> 16) & 0xFFFF;
    
    fat_dir_entry_t* dotdot = (fat_dir_entry_t*)(buffer + sizeof(fat_dir_entry_t));
    memset(dotdot->name, ' ', 11);
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    dotdot->attr = FAT_ATTR_DIRECTORY;
    dotdot->low_cluster = fat_node->start_cluster & 0xFFFF;
    dotdot->high_cluster = (fat_node->start_cluster >> 16) & 0xFFFF;
    
    uint32_t sector = fat_cluster_to_lba(fs, new_cluster);
    fat_write_sector(fs, sector, buffer);
    enhanced_heap_free(buffer, "fat_mkdir");
    
    fat_dir_entry_t dir_entry;
    memset(&dir_entry, 0, sizeof(dir_entry));
    
    const char* basename = name;
    const char* ext = NULL;
    for (int i = strlen(name) - 1; i >= 0; i--) {
        if (name[i] == '.') {
            ext = name + i + 1;
            break;
        }
    }
    
    memset(dir_entry.name, ' ', 11);
    if (ext) {
        int baselen = ext - name;
        if (baselen > 8) baselen = 8;
        memcpy(dir_entry.name, name, baselen);
        int extlen = strlen(ext);
        if (extlen > 3) extlen = 3;
        memcpy(dir_entry.name + 8, ext, extlen);
    } else {
        int len = strlen(name);
        if (len > 8) len = 8;
        memcpy(dir_entry.name, name, len);
    }
    
    dir_entry.attr = FAT_ATTR_DIRECTORY;
    dir_entry.low_cluster = new_cluster & 0xFFFF;
    dir_entry.high_cluster = (new_cluster >> 16) & 0xFFFF;
    
    uint32_t cluster_size = fs->sectors_per_cluster * fs->bytes_per_sector;
    uint8_t* dir_buffer = (uint8_t*)enhanced_heap_alloc(cluster_size, "fat_mkdir2");
    if (!dir_buffer) {
        fat_free_cluster_chain(fs, new_cluster);
        return -1;
    }
    
    uint32_t sector_num = fat_cluster_to_lba(fs, fat_node->start_cluster);
    for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
        fat_read_sector(fs, sector_num + s, dir_buffer + s * fs->bytes_per_sector);
    }
    
    bool found = false;
    for (uint32_t i = 0; i < cluster_size / sizeof(fat_dir_entry_t); i++) {
        fat_dir_entry_t* e = (fat_dir_entry_t*)&dir_buffer[i * sizeof(fat_dir_entry_t)];
        if (e->name[0] == 0 || e->name[0] == FAT_ENTRY_DELETED) {
            memcpy(e, &dir_entry, sizeof(fat_dir_entry_t));
            found = true;
            break;
        }
    }
    
    if (found) {
        for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
            fat_write_sector(fs, sector_num + s, dir_buffer + s * fs->bytes_per_sector);
        }
    }
    
    enhanced_heap_free(dir_buffer, "fat_mkdir2");
    
    return found ? 0 : -1;
}

uint32_t fat_probe(void* dev_data, fat_read_sector_fn read_sector, fat_write_sector_fn write_sector) {
    uint8_t buffer[512];
    
    if (read_sector(dev_data, 0, buffer) != 512) {
        return 0;
    }
    
    if (buffer[510] != 0x55 || buffer[511] != 0xAA) {
        return 0;
    }
    
    uint16_t bytes_per_sector = fs_read16_le(buffer + 11);
    uint8_t sectors_per_cluster = buffer[13];
    uint16_t reserved_sectors = fs_read16_le(buffer + 14);
    uint8_t num_fats = buffer[16];
    uint16_t root_entries = fs_read16_le(buffer + 17);
    uint16_t total_sectors_16 = fs_read16_le(buffer + 19);
    uint8_t media = buffer[21];
    uint16_t sectors_per_fat_16 = fs_read16_le(buffer + 22);
    uint32_t total_sectors_32 = fs_read32_le(buffer + 32);
    
    uint32_t total_sectors = total_sectors_16 ? total_sectors_16 : total_sectors_32;
    uint32_t root_sectors = ((root_entries * 32) + (bytes_per_sector - 1)) / bytes_per_sector;
    uint32_t fat_size = sectors_per_fat_16;
    uint32_t first_data_sector = reserved_sectors + (num_fats * fat_size) + root_sectors;
    uint32_t data_sectors = total_sectors - first_data_sector;
    uint32_t total_clusters = data_sectors / sectors_per_cluster;
    
    uint8_t fat_type;
    if (total_clusters < 4085) {
        fat_type = FAT12;
    } else if (total_clusters < 65525) {
        fat_type = FAT16;
    } else {
        fat_type = FAT32;
    }
    
    debuglog(DEBUG_INFO, "[FAT] Detected FAT%u filesystem\n", fat_type);
    debuglog(DEBUG_INFO, "[FAT]   BPS=%u SPC=%u Res=%u Fats=%u Root=%u\n",
             bytes_per_sector, sectors_per_cluster, reserved_sectors, num_fats, root_entries);
    debuglog(DEBUG_INFO, "[FAT]   Total=%u FatSize=%u Clusters=%u\n",
             total_sectors, fat_size, total_clusters);
    
    return fat_type;
}

bool fat_mount(void* dev_data, fat_read_sector_fn read_sector, fat_write_sector_fn write_sector,
               uint64_t sectors, void** sb_out) {
    if (!dev_data || !sb_out) return false;
    
    fat_fs_t* fs = (fat_fs_t*)enhanced_heap_alloc(sizeof(fat_fs_t), "fat_fs");
    if (!fs) return false;
    
    memset(fs, 0, sizeof(fat_fs_t));
    
    fs->dev_data = dev_data;
    fs->read_sector = read_sector;
    fs->write_sector = write_sector;
    
    uint8_t buffer[512];
    if (fat_read_sector(fs, 0, buffer) != 512) {
        enhanced_heap_free(fs, "fat_fs");
        return false;
    }
    
    fs->bytes_per_sector = fs_read16_le(buffer + 11);
    fs->sectors_per_cluster = buffer[13];
    fs->reserved_sectors = fs_read16_le(buffer + 14);
    fs->num_fats = buffer[16];
    fs->root_entries = fs_read16_le(buffer + 17);
    fs->total_sectors = fs_read16_le(buffer + 19) ? fs_read16_le(buffer + 19) : fs_read32_le(buffer + 32);
    fs->media_descriptor = buffer[21];
    
    if (fs->bytes_per_sector == 0) {
        debuglog(DEBUG_INFO, "[FAT] exFAT detected - not supported yet\n");
        enhanced_heap_free(fs, "fat_fs");
        return false;
    }
    
    uint32_t sectors_per_fat = fs_read16_le(buffer + 22);
    if (sectors_per_fat == 0 && fs->total_sectors > 65535) {
        sectors_per_fat = fs_read32_le(buffer + 36);
    }
    fs->sectors_per_fat = sectors_per_fat;
    
    fs->root_sectors = ((fs->root_entries * 32) + (fs->bytes_per_sector - 1)) / fs->bytes_per_sector;
    fs->data_sectors = fs->total_sectors - fs->reserved_sectors - (fs->num_fats * fs->sectors_per_fat) - fs->root_sectors;
    fs->total_clusters = fs->data_sectors / fs->sectors_per_cluster;
    
    if (fs->total_clusters < 4085) {
        fs->fat_type = FAT12;
    } else if (fs->total_clusters < 65525) {
        fs->fat_type = FAT16;
    } else {
        fs->fat_type = FAT32;
    }
    
    fs->fat_start = fs->reserved_sectors;
    fs->root_start = fs->fat_start + (fs->num_fats * fs->sectors_per_fat);
    fs->data_start = fs->root_start + fs->root_sectors;
    
    g_fat_fs = fs;
    *sb_out = fs;
    
    debuglog(DEBUG_INFO, "[FAT] Mounted FAT%u: fat_start=%u root_start=%u data_start=%u\n",
             fs->fat_type, fs->fat_start, fs->root_start, fs->data_start);
    
    return true;
}

bool fat_umount(void* sb) {
    if (!sb) return false;
    
    fat_fs_t* fs = (fat_fs_t*)sb;
    
    if (fs->dirty && fs->write_sector) {
        debuglog(DEBUG_INFO, "[FAT] Sync: flushing caches\n");
    }
    
    enhanced_heap_free(fs, "fat_fs");
    g_fat_fs = NULL;
    
    return true;
}

vfs_node_t* fat_get_root(void* sb) {
    fat_fs_t* fs = (fat_fs_t*)sb;
    if (!fs) return NULL;
    
    vfs_node_t* node = (vfs_node_t*)enhanced_heap_alloc(sizeof(vfs_node_t), "fat_root");
    if (!node) return NULL;
    
    memset(node, 0, sizeof(vfs_node_t));
    strcpy(node->name, "/");
    node->flags = VFS_DIRECTORY;
    node->inode = 0;
    
    fat_node_t* fat_node = (fat_node_t*)enhanced_heap_alloc(sizeof(fat_node_t), "fat_root_node");
    if (!fat_node) {
        enhanced_heap_free(node, "fat_root");
        return NULL;
    }
    
    memset(fat_node, 0, sizeof(fat_node_t));
    fat_node->fs = fs;
    
    if (fs->fat_type == FAT32) {
        uint8_t buffer[512];
        fat_read_sector(fs, 0, buffer);
        uint32_t root_cluster = fs_read32_le(buffer + 44);
        fat_node->start_cluster = root_cluster;
    } else {
        fat_node->start_cluster = 0;
    }
    
    node->internal_data = fat_node;
    node->open = fat_node_open;
    node->close = fat_node_close;
    node->readdir = fat_node_readdir;
    node->finddir = fat_node_finddir;
    node->unlink = fat_node_unlink;
    node->mkdir = fat_node_mkdir;
    
    return node;
}

typedef struct {
    const char* name;
    uint32_t (*probe)(void*, fat_read_sector_fn, fat_write_sector_fn);
    bool (*mount)(void*, fat_read_sector_fn, fat_write_sector_fn, uint64_t, void**);
    bool (*umount)(void*);
    vfs_node_t* (*get_root)(void*);
} fat_filesystem_t;

static fat_filesystem_t fat_fs_ops = {
    .name = "fat",
    .probe = fat_probe,
    .mount = fat_mount,
    .umount = fat_umount,
    .get_root = fat_get_root,
};

int fat_register(void) {
    vfs_filesystem_t* vfs_fs = (vfs_filesystem_t*)enhanced_heap_alloc(sizeof(vfs_filesystem_t), "fat_vfs_reg");
    if (!vfs_fs) return -1;
    
    memset(vfs_fs, 0, sizeof(vfs_filesystem_t));
    vfs_fs->name = "fat";
    vfs_fs->probe = (uint32_t(*)(void*, void*, void*))fat_probe;
    vfs_fs->mount = (bool(*)(void*, void*, void*, uint64_t, void**))fat_mount;
    vfs_fs->umount = (bool(*)(void*))fat_umount;
    vfs_fs->get_root = (vfs_node_t*(*)(void*))fat_get_root;
    
    return vfs_register_filesystem(vfs_fs);
}
