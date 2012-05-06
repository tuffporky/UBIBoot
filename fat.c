
#include <stdint.h>
#include <string.h>

#include "serial.h"
#include "mmc.h"
#include "fat.h"
#include "config.h"

static struct boot_sector bs;

static int get_first_partition(uint32_t *lba)
{
	struct mbr mbr;

	if (mmc_block_read((uint8_t *) &mbr, 0, 1)) {
		SERIAL_PUTS("MMC: Unable to read bootsector.\n");
		return -1;
	}

	if (mbr.signature != 0xAA55) {
		SERIAL_PUTS("MMC: No MBR detected.\n");
		return -1;
	}

	if (mbr.partitions[0].status
				&& mbr.partitions[0].status != 0x80) {
		SERIAL_PUTS("MMC: Unable to detect first physical partition.\n");
		return -1;
	}

	*lba =  mbr.partitions[0].lba;
	return 0;
}

static int load_from_cluster(uint32_t lba, uint32_t cluster, unsigned char *ld_addr)
{
	uint32_t old_fat_sector = 0;

	while(1) {
		uint8_t sector[FAT_BLOCK_SIZE];
		uint32_t fat_sector = lba + bs.reserved + (cluster / (FAT_BLOCK_SIZE >> 2));

		/* Read file data */
		if (mmc_block_read(ld_addr, lba + bs.reserved + bs.fat32_length * bs.fats
						+ (cluster-2) * bs.cluster_size, bs.cluster_size)) {
			SERIAL_PUTS("MMC: Unable to read from first partition.\n");
			return -1;
		}
		ld_addr += bs.cluster_size * FAT_BLOCK_SIZE;

		/* Read FAT */
		if (fat_sector != old_fat_sector) {
			if (mmc_block_read(sector, fat_sector, 1)) {
				SERIAL_PUTS("MMC: Unable to read the FAT table.\n");
				return -1;
			}
			old_fat_sector = fat_sector;
		}

		cluster = ((uint32_t *)sector) [cluster % (FAT_BLOCK_SIZE >> 2)] & 0x0fffffff;
		if ((cluster >= 0x0ffffff0) || (cluster <= 1))
			break;
	}

	return 0;
}

static int load_kernel_lba(uint32_t lba, unsigned char *ld_addr)
{
	uint8_t sector[FAT_BLOCK_SIZE];
	uint32_t cur_sect;
	size_t i, j;
	struct volume_info vinfo;

	if (mmc_block_read(sector, lba, 1)) {
		SERIAL_PUTS("MMC: Unable to read from first partition.\n");
		return -1;
	}

	memcpy(&bs, sector, sizeof(struct boot_sector));
	memcpy(&vinfo, (void *) sector + sizeof(struct boot_sector),
				sizeof(struct volume_info));

	if (strncmp(vinfo.fs_type, "FAT32", 5)) {
		SERIAL_PUTS("MMC: No FAT32 filesystem detected!\n");
		return -1;
	}

	SERIAL_PUTS("MMC: FAT32 filesystem detected.\n");

	for (cur_sect = lba + bs.reserved + bs.fat32_length * bs.fats, i = 0;
				i < bs.cluster_size; cur_sect++, i++) {
		struct dir_entry *entry;

		/* Read one sector */
		if (mmc_block_read(sector, cur_sect, 1)) {
			SERIAL_PUTS("MMC: Unable to read rootdir sector.\n");
			return -1;
		}

		/* Read all file handles from this sector */
		for (entry = (struct dir_entry *) sector, j = 0;
					j < FAT_BLOCK_SIZE / sizeof(struct dir_entry);
					entry++, j++) {
			char c;

			if (entry->attr & (ATTR_VOLUME | ATTR_DIR))
				continue;

			if (entry->name[0] == 0xe5)
				continue;

			if (!entry->name[0]) {
				SERIAL_PUTS("MMC: Kernel file not found...\n");
				return -1;
			}

			if (strncmp(entry->name, FAT_BOOTFILE_NAME,
							sizeof(FAT_BOOTFILE_NAME) - 1)
						|| strncmp(entry->ext, FAT_BOOTFILE_EXT,
							sizeof(FAT_BOOTFILE_EXT) - 1))
				continue;

			c = entry->name[sizeof(FAT_BOOTFILE_NAME) - 1];
			if (c && c != ' ')
				continue;

			SERIAL_PUTS("MMC: Loading kernel file...\n");
			return load_from_cluster(lba,
						entry->starthi << 16
						| entry->start, ld_addr);
		}
	}

	SERIAL_PUTS("MMC: Kernel file not found...\n");
	return -1;
}

int mmc_load_kernel(unsigned char *ld_addr)
{
	uint32_t lba = 0;
	get_first_partition(&lba);

	SERIAL_PUTS("MMC: First partition starts at LBA ");
	SERIAL_PUTI(lba);
	SERIAL_PUTC('\n');

	return load_kernel_lba(lba, ld_addr);
}
