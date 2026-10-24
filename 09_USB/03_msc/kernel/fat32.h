#ifndef F_H
#define F_H

#include <linux/types.h>
#include "usb1.h"
#include "mass_storage.h"

/*
 * Directory entry attribute values (kept for compatibility with older code).
 * The driver itself tests attributes with masks, not with '=='.
 */
#define FILE_TYPE    0x20
#define DIR_TYPE     0x10
#define LFN_TYPE     0x0F
#define CONTENT_TYPE 0x21   /* pseudo type, only used for tree printing */

#define SEQ_NUM    0x1F     /* LFN sequence number, 5 bits */
#define END_MARKER 0x40     /* LFN "last logical entry" flag, bit 6 */

/* Path limits */
#define MAX_PATH_LEN      256
#define MAX_FULL_PATH_LEN (MAX_PATH_LEN - 3)

/* Kept for compatibility with callers */
#define PRINT_DIR        1
#define NO_PRINT_DIR     0
#define ALL_DIR_READ_ALL (1u << 2)
#define ALL_DIR          1
#define CURRENT_DIR      0
#define PRINT_DATA       1
#define NO_PRINT_DATA    0

struct usb_device_data;

/* BIOS Parameter Block (only the fields the driver needs) */
typedef struct BPB {
	u32 Starting_LBA;        /* LBA of the volume boot record (partition start) */
	u8  OME_name[9];
	u8  Partition_type;
	u16 Bytes_per_Sector;
	u8  Sectors_per_Cluster;
	u16 Reserved_Sectors;
	u16 Number_of_FATs;
	u32 Sectors_per_FAT;
	u8  FS_type[9];
	u32 Root_Cluster;
	u32 Data_Sector;         /* first data sector, relative to Starting_LBA */
	u32 Total_Sectors;       /* sectors in the volume */
	u16 FSInfo_Sector;       /* relative to Starting_LBA */
	u32 Cluster_Count;       /* number of data clusters */
	u32 Max_Cluster;         /* highest valid cluster number */
} BPB;

/* 32-byte short-name directory entry (layout reference) */
typedef struct Root_Directory_Entry {
	u8  id;
	u8  File_name[10];
	u8  File_attributes;
	u8  Reserved;
	u8  File_create_time;
	u16 Create_time;
	u16 Create_date;
	u16 Access_date;
	u16 High_first_cluster;
	u16 Modified_time;
	u16 Modified_date;
	u16 Low_first_cluster;
	u32 File_size;
} Root_Dir_Entry;

/* 32-byte long-name directory entry (layout reference) */
typedef struct LFN_Root_Directory_Entry {
	u8  id;
	u8  File_name1[10];
	u8  File_attributes;
	u8  type;
	u8  checksum;
	u8  File_name2[12];
	u16 Reserved;
	u8  File_name3[4];
} LFN_Root_Dir_Entry;

/* Raw cluster I/O. cluster_data must hold Sectors_per_Cluster * 512 bytes. */
int USB1_Read_CLUSTER(struct usb_device_data *data, u32 cluster_num,
		      u8 *cluster_data, u32 *cluster_data_len, u8 *name,
		      bool print_data);
int USB1_Write_CLUSTER(struct usb_device_data *data, u32 cluster_num,
		       u8 *cluster_data, u8 *name, bool print_data);

/* FAT32 driver API */
int USB1_f_Mount(struct usb_device_data *data);
int USB1_f_Read_All(struct usb_device_data *data);
int USB1_f_Read_Dir(struct usb_device_data *data, u8 *path_dir);
int USB1_f_Read_File(struct usb_device_data *data, u8 *path_file);
int USB1_f_Make_Dir(struct usb_device_data *data, u8 *path_dir);
int USB1_f_Remove_Dir(struct usb_device_data *data, u8 *path_dir);
int USB1_f_Make_File(struct usb_device_data *data, u8 *path_file);
int USB1_f_Remove_File(struct usb_device_data *data, u8 *path_file);
int USB1_f_Add_Content(struct usb_device_data *data, u8 *path_file, u8 *content);
int USB1_f_Delete_Content(struct usb_device_data *data, u8 *path_file);
int USB1_f_Clear_All(struct usb_device_data *data);

#endif /* F_H */
