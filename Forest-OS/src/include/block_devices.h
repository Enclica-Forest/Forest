#ifndef BLOCK_DEVICES_H
#define BLOCK_DEVICES_H

// Registers real block devices (/dev/sd*, /dev/loop*) backed by detected ATA
// hardware and their MBR partitions. Call after ata_init()+ata_detect_devices()
// and after devfs_init(). Returns 0 on success.
int block_devices_init_real(void);
void block_devices_cleanup_real(void);

#endif
