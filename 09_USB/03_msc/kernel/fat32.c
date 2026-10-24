// SPDX-License-Identifier: GPL-2.0
/*
 * fat32.c - small FAT32 driver on top of the custom USB MSC layer.
 *
 * Design rules (they fix the cross-linked cluster / directory loop problem):
 *   - Every FAT update goes to ALL FAT copies and keeps the top 4 bits.
 *   - The allocator searches the whole FAT and never returns an in-use cluster.
 *   - A cluster is allocated only after "already exists" checks pass.
 *   - Directory clusters are zeroed when allocated.
 *   - Name matching is exact (case-insensitive), first match only.
 *   - Removing a directory frees its whole subtree and every cluster chain.
 *   - Directory scans are sector based, depth limited and loop guarded.
 *   - Cluster size comes from the BPB (Sectors_per_Cluster), never a constant.
 */

#include "fat32.h"
#include "main.h"
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/mutex.h>
#include <linux/mm.h>
#include <linux/time.h>
#include <linux/timekeeping.h>
#include <asm/unaligned.h>

/* ------------------------------------------------------------------ */
/* Constants                                                          */
/* ------------------------------------------------------------------ */

#define DIR_ENT_SIZE        32
#define DIR_ENTS_PER_SEC    (SECTOR_SIZE / DIR_ENT_SIZE)
#define FAT_ENTS_PER_SEC    (SECTOR_SIZE / 4)

#define FAT32_MASK          0x0FFFFFFFU
#define FAT32_EOC           0x0FFFFFFFU

#define FAT_NAME_MAX        255
#define FAT_LFN_MAX         20      /* ceil(255 / 13) */
#define FAT_CHARS_PER_LFN   13
#define FAT_MAX_DEPTH       8       /* recursion limit (8 KB ARM kernel stack) */
#define FAT_PRINT_MAX       4096    /* max file bytes printed by Read_* */
#define FAT_CONTENT_MAX     (1024 * 1024)

#define ATTR_READ_ONLY      0x01
#define ATTR_HIDDEN         0x02
#define ATTR_SYSTEM         0x04
#define ATTR_VOLUME_ID      0x08
#define ATTR_DIRECTORY      0x10
#define ATTR_ARCHIVE        0x20
#define ATTR_LFN            0x0F

#define DIRENT_DELETED      0xE5
#define DIRENT_END          0x00

#define PAD_BUF_SIZE        ((FAT_MAX_DEPTH + 3) * 3 + 8)

#if (PRINT_DEBUG)
#define fat_dbg(fmt, ...) printk(KERN_INFO "FAT32: " fmt, ##__VA_ARGS__)
#else
#define fat_dbg(fmt, ...) do { } while (0)
#endif

/* ------------------------------------------------------------------ */
/* State                                                              */
/* ------------------------------------------------------------------ */

BPB bpb_instance;

static DEFINE_MUTEX(fat_mutex);

static u8  sec_buf[SECTOR_SIZE];      /* dir-entry patches, file data, mount */
static u8  fat_scratch[SECTOR_SIZE];  /* FAT copies >= 1, FSInfo */
static u8  zero_sec[SECTOR_SIZE];     /* always zero */
static u8  fat_cache[SECTOR_SIZE];    /* cached sector of FAT #0 */
static u32 fat_cache_lba = U32_MAX;
static u32 alloc_hint = 3;
static bool fsinfo_invalidated;

/* Byte offsets of the 13 UCS-2 characters inside an LFN entry */
static const u8 lfn_char_off[FAT_CHARS_PER_LFN] = {
	1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30
};

/* Parsed directory entry */
struct fat_dirent {
	char name[FAT_NAME_MAX + 1];  /* long name, or 8.3 name if no valid LFN */
	u8   sfn[11];
	u8   attr;
	u32  cluster;
	u32  size;
	u32  sfn_lba;
	u16  sfn_off;
	u8   lfn_cnt;
	u32  lfn_lba[FAT_LFN_MAX];
	u16  lfn_off[FAT_LFN_MAX];
};

/* Directory iterator: walks one directory sector by sector */
struct fat_iter {
	u32  cluster;
	u32  sec;           /* sector inside cluster */
	u32  ent;           /* entry inside sector */
	u32  lba;
	u32  hops;          /* clusters visited, loop guard */
	bool loaded;
	bool end;
	u8   buf[SECTOR_SIZE];

	/* LFN assembly */
	bool lfn_ok;
	u8   lfn_next;
	u8   lfn_chk;
	u8   lfn_cnt;
	u16  lfn_name[FAT_LFN_MAX * FAT_CHARS_PER_LFN + 1];
	u32  lfn_lba[FAT_LFN_MAX];
	u16  lfn_off[FAT_LFN_MAX];
};

/* ------------------------------------------------------------------ */
/* Sector / cluster helpers                                           */
/* ------------------------------------------------------------------ */

static int sec_read(struct usb_device_data *data, u32 lba, u8 *buf)
{
	u16 len = 0;

	if (USB1_Read_SECTOR_DATA(data, lba, 1, buf, &len, (u8 *)"FAT32 read",
				  0, NO_PRINT_DATA) < 0)
		return -EIO;
	return 0;
}

static int sec_write(struct usb_device_data *data, u32 lba, const u8 *buf)
{
	if (USB1_Write_SECTOR_DATA(data, lba, 1, (u8 *)buf, SECTOR_SIZE,
				   (u8 *)"FAT32 write", 0, NO_PRINT_DATA) < 0)
		return -EIO;
	return 0;
}

static inline bool clu_valid(u32 cl)
{
	return cl >= 2 && cl <= bpb_instance.Max_Cluster;
}

static inline u32 clu_lba(u32 cl)
{
	return bpb_instance.Starting_LBA + bpb_instance.Data_Sector +
	       (cl - 2) * bpb_instance.Sectors_per_Cluster;
}

static inline u32 cluster_bytes(void)
{
	return (u32)bpb_instance.Sectors_per_Cluster * SECTOR_SIZE;
}

static int zero_cluster(struct usb_device_data *data, u32 cl)
{
	u32 s;
	int ret;

	for (s = 0; s < bpb_instance.Sectors_per_Cluster; s++) {
		ret = sec_write(data, clu_lba(cl) + s, zero_sec);
		if (ret)
			return ret;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Public raw cluster I/O (uses the real cluster size)                */
/* ------------------------------------------------------------------ */

int USB1_Read_CLUSTER(struct usb_device_data *data, u32 cluster_num,
		      u8 *cluster_data, u32 *cluster_data_len, u8 *name,
		      bool print_data)
{
	u32 s;
	int ret;

	*cluster_data_len = 0;
	if (!clu_valid(cluster_num))
		return -EINVAL;

	for (s = 0; s < bpb_instance.Sectors_per_Cluster; s++) {
		ret = sec_read(data, clu_lba(cluster_num) + s,
			       cluster_data + s * SECTOR_SIZE);
		if (ret)
			return ret;
		*cluster_data_len += SECTOR_SIZE;
	}
	if (print_data)
		USB1_Print_Hex(cluster_data, SECTOR_SIZE, name);
	return 0;
}

int USB1_Write_CLUSTER(struct usb_device_data *data, u32 cluster_num,
		       u8 *cluster_data, u8 *name, bool print_data)
{
	u32 s;
	int ret;

	if (!clu_valid(cluster_num))
		return -EINVAL;

	for (s = 0; s < bpb_instance.Sectors_per_Cluster; s++) {
		ret = sec_write(data, clu_lba(cluster_num) + s,
				cluster_data + s * SECTOR_SIZE);
		if (ret)
			return ret;
	}
	if (print_data)
		USB1_Print_Hex(cluster_data, SECTOR_SIZE, name);
	return 0;
}

/* ------------------------------------------------------------------ */
/* FAT table                                                          */
/* ------------------------------------------------------------------ */

/* FSInfo free-count / next-free are hints; mark them unknown once we write. */
static int fsinfo_invalidate(struct usb_device_data *data)
{
	u32 lba;
	int ret;

	if (fsinfo_invalidated)
		return 0;
	fsinfo_invalidated = true;

	if (bpb_instance.FSInfo_Sector == 0 || bpb_instance.FSInfo_Sector == 0xFFFF)
		return 0;

	lba = bpb_instance.Starting_LBA + bpb_instance.FSInfo_Sector;
	ret = sec_read(data, lba, fat_scratch);
	if (ret)
		return ret;

	if (get_unaligned_le32(fat_scratch) != 0x41615252 ||
	    get_unaligned_le32(fat_scratch + 484) != 0x61417272)
		return 0;

	put_unaligned_le32(0xFFFFFFFF, fat_scratch + 488);   /* free count unknown */
	put_unaligned_le32(0xFFFFFFFF, fat_scratch + 492);   /* next free unknown  */
	return sec_write(data, lba, fat_scratch);
}

static int fat_get(struct usb_device_data *data, u32 cl, u32 *val)
{
	u32 lba;
	int ret;

	if (!clu_valid(cl))
		return -EINVAL;

	lba = bpb_instance.Starting_LBA + bpb_instance.Reserved_Sectors +
	      cl / FAT_ENTS_PER_SEC;
	if (lba != fat_cache_lba) {
		ret = sec_read(data, lba, fat_cache);
		if (ret) {
			fat_cache_lba = U32_MAX;
			return ret;
		}
		fat_cache_lba = lba;
	}
	*val = get_unaligned_le32(fat_cache + (cl % FAT_ENTS_PER_SEC) * 4) &
	       FAT32_MASK;
	return 0;
}

/* Write one FAT entry in every FAT copy, keeping the reserved top 4 bits. */
static int fat_set(struct usb_device_data *data, u32 cl, u32 val)
{
	u32 copy, lba, old;
	u32 off = (cl % FAT_ENTS_PER_SEC) * 4;
	u8 *buf;
	int ret;

	if (!clu_valid(cl))
		return -EINVAL;

	ret = fsinfo_invalidate(data);
	if (ret)
		return ret;

	for (copy = 0; copy < bpb_instance.Number_of_FATs; copy++) {
		lba = bpb_instance.Starting_LBA + bpb_instance.Reserved_Sectors +
		      copy * bpb_instance.Sectors_per_FAT + cl / FAT_ENTS_PER_SEC;

		if (copy == 0) {
			buf = fat_cache;
			if (fat_cache_lba != lba) {
				ret = sec_read(data, lba, fat_cache);
				if (ret) {
					fat_cache_lba = U32_MAX;
					return ret;
				}
				fat_cache_lba = lba;
			}
		} else {
			buf = fat_scratch;
			ret = sec_read(data, lba, buf);
			if (ret)
				return ret;
		}

		old = get_unaligned_le32(buf + off);
		put_unaligned_le32((old & ~FAT32_MASK) | (val & FAT32_MASK),
				   buf + off);

		ret = sec_write(data, lba, buf);
		if (ret) {
			if (copy == 0)
				fat_cache_lba = U32_MAX;
			return ret;
		}
	}
	return 0;
}

/* Free a whole chain. Never frees the root directory cluster. */
static int fat_free_chain(struct usb_device_data *data, u32 cl)
{
	u32 next, hops = 0;
	int ret;

	while (clu_valid(cl) && cl != bpb_instance.Root_Cluster) {
		ret = fat_get(data, cl, &next);
		if (ret)
			return ret;
		if (next == 0)              /* already free: stop, avoid wandering */
			break;
		ret = fat_set(data, cl, 0);
		if (ret)
			return ret;
		if (cl < alloc_hint)
			alloc_hint = cl;
		cl = next;
		if (++hops > bpb_instance.Max_Cluster)
			return -ELOOP;
	}
	return 0;
}

/* Allocate one free cluster, mark it end-of-chain, optionally zero it. */
static int fat_alloc(struct usb_device_data *data, u32 *out, bool zero)
{
	u32 start, cl, val;
	int ret;

	start = clu_valid(alloc_hint) ? alloc_hint : 2;
	cl = start;
	do {
		ret = fat_get(data, cl, &val);
		if (ret)
			return ret;
		if (val == 0)
			goto found;
		cl = (cl >= bpb_instance.Max_Cluster) ? 2 : cl + 1;
	} while (cl != start);

	printk(KERN_ERR "FAT32: no free cluster\n");
	return -ENOSPC;

found:
	ret = fat_set(data, cl, FAT32_EOC);
	if (ret)
		return ret;

	if (zero) {
		ret = zero_cluster(data, cl);
		if (ret) {
			fat_set(data, cl, 0);
			return ret;
		}
	}

	alloc_hint = (cl >= bpb_instance.Max_Cluster) ? 2 : cl + 1;
	*out = cl;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Names                                                              */
/* ------------------------------------------------------------------ */

static u8 lfn_checksum(const u8 *sfn)
{
	u8 sum = 0;
	int i;

	for (i = 0; i < 11; i++)
		sum = ((sum & 1) ? 0x80 : 0) + (sum >> 1) + sfn[i];
	return sum;
}

/* 8.3 directory form -> "NAME.EXT" (honours the NT lowercase flags). */
static void sfn_to_name(const u8 *sfn, u8 nt_flags, char *out)
{
	int i, n = 0, b, e;
	u8 c;

	for (b = 8; b > 0 && sfn[b - 1] == ' '; b--)
		;
	for (e = 3; e > 0 && sfn[8 + e - 1] == ' '; e--)
		;

	for (i = 0; i < b; i++) {
		c = sfn[i];
		if (i == 0 && c == 0x05)
			c = 0xE5;
		if (nt_flags & 0x08)
			c = tolower(c);
		out[n++] = c;
	}
	if (e) {
		out[n++] = '.';
		for (i = 0; i < e; i++) {
			c = sfn[8 + i];
			if (nt_flags & 0x10)
				c = tolower(c);
			out[n++] = c;
		}
	}
	out[n] = '\0';
}

static int valid_name(const char *n)
{
	size_t l = strlen(n);
	const char *s;

	if (l == 0)
		return -EINVAL;
	if (l > FAT_NAME_MAX)
		return -ENAMETOOLONG;
	if (!strcmp(n, ".") || !strcmp(n, ".."))
		return -EINVAL;
	if (n[l - 1] == '.' || n[l - 1] == ' ')
		return -EINVAL;

	for (s = n; *s; s++) {
		u8 c = (u8)*s;

		if (c < 0x20 || c >= 0x80)
			return -EINVAL;           /* ASCII only in this driver */
		if (strchr("\\/:*?\"<>|", c))
			return -EINVAL;
	}
	return 0;
}

/* Convert one character for a short name. Returns 0 to drop it. */
static u8 sfn_char(char ch, bool *lossy)
{
	u8 c = (u8)ch;

	if (c == ' ') {
		*lossy = true;
		return 0;
	}
	if (isalnum(c) || strchr("$%'-_@~`!(){}^#&", c))
		return toupper(c);

	*lossy = true;                       /* '.', '+', ',', ';', '=', '[', ']' */
	return '_';
}

static inline bool is_dot(const struct fat_dirent *de)
{
	return !strcmp(de->name, ".") || !strcmp(de->name, "..");
}

/* ------------------------------------------------------------------ */
/* Directory iteration                                                */
/* ------------------------------------------------------------------ */

static struct fat_iter *iter_new(u32 dir_cl)
{
	struct fat_iter *it = kzalloc(sizeof(*it), GFP_KERNEL);

	if (!it)
		return NULL;
	it->cluster = dir_cl ? dir_cl : bpb_instance.Root_Cluster;
	return it;
}

static int iter_advance(struct usb_device_data *data, struct fat_iter *it)
{
	u32 next;
	int ret;

	if (++it->ent < DIR_ENTS_PER_SEC)
		return 0;
	it->ent = 0;
	it->loaded = false;

	if (++it->sec < bpb_instance.Sectors_per_Cluster)
		return 0;
	it->sec = 0;

	ret = fat_get(data, it->cluster, &next);
	if (ret)
		return ret;
	if (!clu_valid(next)) {         /* EOC, bad or free: end of directory */
		it->end = true;
		return 0;
	}
	if (++it->hops > bpb_instance.Max_Cluster)
		return -ELOOP;
	it->cluster = next;
	return 0;
}

/* Raw slot access. Returns 1 with a slot, 0 at end of chain, <0 on error. */
static int iter_raw(struct usb_device_data *data, struct fat_iter *it,
		    u8 **e, u32 *lba, u16 *off)
{
	int ret;

	if (it->end)
		return 0;

	if (!it->loaded) {
		if (!clu_valid(it->cluster))
			return -EIO;
		it->lba = clu_lba(it->cluster) + it->sec;
		ret = sec_read(data, it->lba, it->buf);
		if (ret)
			return ret;
		it->loaded = true;
	}

	*e = it->buf + it->ent * DIR_ENT_SIZE;
	*lba = it->lba;
	*off = it->ent * DIR_ENT_SIZE;

	/* *e stays valid: the buffer is only reloaded on the next call */
	ret = iter_advance(data, it);
	return ret < 0 ? ret : 1;
}

/* Next real entry (files and dirs, including "." and ".."). */
static int iter_next(struct usb_device_data *data, struct fat_iter *it,
		     struct fat_dirent *de)
{
	u8 *e;
	u32 lba;
	u16 off;
	int ret, i, j, seq;

	for (;;) {
		ret = iter_raw(data, it, &e, &lba, &off);
		if (ret <= 0)
			return ret;

		if (e[0] == DIRENT_END) {
			it->end = true;
			return 0;
		}
		if (e[0] == DIRENT_DELETED) {
			it->lfn_ok = false;
			continue;
		}

		if ((e[11] & 0x3F) == ATTR_LFN) {
			seq = e[0] & SEQ_NUM;
			if (e[0] & END_MARKER) {
				if (seq == 0 || seq > FAT_LFN_MAX) {
					it->lfn_ok = false;
					continue;
				}
				it->lfn_ok = true;
				it->lfn_next = seq;
				it->lfn_chk = e[13];
				it->lfn_cnt = 0;
				memset(it->lfn_name, 0, sizeof(it->lfn_name));
			}
			if (!it->lfn_ok || seq != it->lfn_next ||
			    e[13] != it->lfn_chk) {
				it->lfn_ok = false;
				continue;
			}
			for (j = 0; j < FAT_CHARS_PER_LFN; j++)
				it->lfn_name[(seq - 1) * FAT_CHARS_PER_LFN + j] =
					get_unaligned_le16(e + lfn_char_off[j]);
			it->lfn_lba[it->lfn_cnt] = lba;
			it->lfn_off[it->lfn_cnt] = off;
			it->lfn_cnt++;
			it->lfn_next--;
			continue;
		}

		if (e[11] & ATTR_VOLUME_ID) {
			it->lfn_ok = false;
			continue;
		}

		/* Short-name entry */
		memcpy(de->sfn, e, 11);
		de->attr = e[11];
		de->cluster = (((u32)get_unaligned_le16(e + 20) << 16) |
			       get_unaligned_le16(e + 26)) & FAT32_MASK;
		de->size = get_unaligned_le32(e + 28);
		de->sfn_lba = lba;
		de->sfn_off = off;
		de->lfn_cnt = 0;

		if (it->lfn_ok && it->lfn_next == 0 &&
		    it->lfn_chk == lfn_checksum(e)) {
			for (i = 0; i < FAT_NAME_MAX; i++) {
				u16 c = it->lfn_name[i];

				if (c == 0x0000 || c == 0xFFFF)
					break;
				de->name[i] = (c < 0x80) ? (char)c : '?';
			}
			de->name[i] = '\0';
			if (i == 0)
				sfn_to_name(e, e[12], de->name);
			de->lfn_cnt = it->lfn_cnt;
			memcpy(de->lfn_lba, it->lfn_lba, sizeof(de->lfn_lba));
			memcpy(de->lfn_off, it->lfn_off, sizeof(de->lfn_off));
		} else {
			sfn_to_name(e, e[12], de->name);
		}

		it->lfn_ok = false;
		return 1;
	}
}

/* Exact (case-insensitive) lookup. 0 = found, -ENOENT, or error. */
static int dir_lookup(struct usb_device_data *data, u32 dir_cl,
		      const char *name, struct fat_dirent *de)
{
	struct fat_iter *it = iter_new(dir_cl);
	int ret;

	if (!it)
		return -ENOMEM;

	while ((ret = iter_next(data, it, de)) > 0) {
		if (is_dot(de))
			continue;
		if (!strcasecmp(de->name, name)) {
			ret = 0;
			goto out;
		}
	}
	if (ret == 0)
		ret = -ENOENT;
out:
	kfree(it);
	return ret;
}

/* 1 if an entry with this 8.3 name exists, 0 if not, <0 on error. */
static int sfn_exists(struct usb_device_data *data, u32 dir_cl, const u8 *sfn)
{
	struct fat_iter *it = iter_new(dir_cl);
	struct fat_dirent *de = kmalloc(sizeof(*de), GFP_KERNEL);
	int ret;

	if (!it || !de) {
		ret = -ENOMEM;
		goto out;
	}
	while ((ret = iter_next(data, it, de)) > 0) {
		if (!memcmp(de->sfn, sfn, 11)) {
			ret = 1;
			goto out;
		}
	}
out:
	kfree(de);
	kfree(it);
	return ret;
}

/* Build a unique 8.3 name for 'name' inside directory dir_cl. */
static int make_sfn(struct usb_device_data *data, u32 dir_cl,
		    const char *name, u8 *sfn)
{
	u8 b[8], x[3], c;
	int bl = 0, xl = 0, tl, keep, ret;
	unsigned int n;
	bool lossy = false;
	const char *p = name, *dot, *s;
	char tail[9];

	while (*p == '.' || *p == ' ') {     /* ".Hidden_item" -> "HIDDEN~1" */
		p++;
		lossy = true;
	}
	dot = strrchr(p, '.');

	for (s = p; *s && s != dot; s++) {
		c = sfn_char(*s, &lossy);
		if (!c)
			continue;
		if (bl < 8)
			b[bl++] = c;
		else
			lossy = true;
	}
	if (dot) {
		for (s = dot + 1; *s; s++) {
			c = sfn_char(*s, &lossy);
			if (!c)
				continue;
			if (xl < 3)
				x[xl++] = c;
			else
				lossy = true;
		}
	}
	if (bl == 0) {
		b[bl++] = '_';
		lossy = true;
	}

	memset(sfn, ' ', 11);
	memcpy(sfn, b, bl);
	memcpy(sfn + 8, x, xl);

	if (!lossy) {
		ret = sfn_exists(data, dir_cl, sfn);
		if (ret <= 0)
			return ret;             /* 0: free name, <0: error */
	}

	/* Numeric tail: BASE~1, BASE~2, ... */
	for (n = 1; n < 1000000; n++) {
		tl = snprintf(tail, sizeof(tail), "~%u", n);
		keep = min(bl, 8 - tl);
		memset(sfn, ' ', 8);
		memcpy(sfn, b, keep);
		memcpy(sfn + keep, tail, tl);

		ret = sfn_exists(data, dir_cl, sfn);
		if (ret <= 0)
			return ret;
	}
	return -EEXIST;
}

/* ------------------------------------------------------------------ */
/* Directory entry construction and writing                           */
/* ------------------------------------------------------------------ */

static void fat_now(u16 *t, u16 *d)
{
	struct tm tm;
	long year;

	time64_to_tm(ktime_get_real_seconds(), 0, &tm);
	year = tm.tm_year + 1900;
	if (year < 1980) {
		*d = (0 << 9) | (1 << 5) | 1;     /* 1980-01-01 */
		*t = 0;
		return;
	}
	*d = ((year - 1980) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday;
	*t = (tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2);
}

static void fill_sfn(u8 *e, const u8 *sfn, u8 attr, u32 cluster, u32 size)
{
	u16 t, d;

	fat_now(&t, &d);
	memset(e, 0, DIR_ENT_SIZE);
	memcpy(e, sfn, 11);
	e[11] = attr;
	put_unaligned_le16(t, e + 14);                 /* create time   */
	put_unaligned_le16(d, e + 16);                 /* create date   */
	put_unaligned_le16(d, e + 18);                 /* access date   */
	put_unaligned_le16(cluster >> 16, e + 20);     /* cluster high  */
	put_unaligned_le16(t, e + 22);                 /* modified time */
	put_unaligned_le16(d, e + 24);                 /* modified date */
	put_unaligned_le16(cluster & 0xFFFF, e + 26);  /* cluster low   */
	put_unaligned_le32(size, e + 28);
}

/* LFN entries (physical order) followed by the SFN entry. Returns count. */
static int build_entries(const char *name, const u8 *sfn, u8 attr,
			 u32 cluster, u32 size, u8 *ents)
{
	int len = strlen(name);
	int nlfn = DIV_ROUND_UP(len, FAT_CHARS_PER_LFN);
	u8 chk = lfn_checksum(sfn);
	int i, j, seq, idx;
	u16 c;
	u8 *e;

	for (i = 0; i < nlfn; i++) {
		seq = nlfn - i;
		e = ents + i * DIR_ENT_SIZE;
		memset(e, 0, DIR_ENT_SIZE);
		e[0] = seq | (i == 0 ? END_MARKER : 0);
		e[11] = ATTR_LFN;
		e[12] = 0;
		e[13] = chk;
		for (j = 0; j < FAT_CHARS_PER_LFN; j++) {
			idx = (seq - 1) * FAT_CHARS_PER_LFN + j;
			if (idx < len)
				c = (u8)name[idx];
			else if (idx == len)
				c = 0x0000;
			else
				c = 0xFFFF;
			put_unaligned_le16(c, e + lfn_char_off[j]);
		}
	}
	fill_sfn(ents + nlfn * DIR_ENT_SIZE, sfn, attr, cluster, size);
	return nlfn + 1;
}

/* Write n 32-byte entries to the given slots (read-modify-write per sector). */
static int write_entries(struct usb_device_data *data, const u32 *lba,
			 const u16 *off, const u8 *ents, int n)
{
	u32 cur = U32_MAX;
	int i, ret;

	for (i = 0; i < n; i++) {
		if (lba[i] != cur) {
			if (cur != U32_MAX) {
				ret = sec_write(data, cur, sec_buf);
				if (ret)
					return ret;
			}
			ret = sec_read(data, lba[i], sec_buf);
			if (ret)
				return ret;
			cur = lba[i];
		}
		memcpy(sec_buf + off[i], ents + i * DIR_ENT_SIZE, DIR_ENT_SIZE);
	}
	return (cur != U32_MAX) ? sec_write(data, cur, sec_buf) : 0;
}

/* Find n consecutive free slots; extend the directory if needed. */
static int find_free_slots(struct usb_device_data *data, u32 dir_cl, int n,
			   u32 *lba, u16 *off)
{
	struct fat_iter *it = iter_new(dir_cl);
	u32 last, nc, s, k;
	int run = 0, ret;
	u8 *e;
	u32 l;
	u16 o;

	if (!it)
		return -ENOMEM;

	while ((ret = iter_raw(data, it, &e, &l, &o)) > 0) {
		if (e[0] == DIRENT_END || e[0] == DIRENT_DELETED) {
			lba[run] = l;
			off[run] = o;
			if (++run == n) {
				ret = 0;
				goto out;
			}
		} else {
			run = 0;
		}
	}
	if (ret < 0)
		goto out;

	/* Directory full: append zeroed clusters until the run fits */
	last = it->cluster;
	while (run < n) {
		ret = fat_alloc(data, &nc, true);
		if (ret)
			goto out;
		ret = fat_set(data, last, nc);
		if (ret) {
			fat_set(data, nc, 0);
			goto out;
		}
		for (s = 0; s < bpb_instance.Sectors_per_Cluster && run < n; s++)
			for (k = 0; k < DIR_ENTS_PER_SEC && run < n; k++) {
				lba[run] = clu_lba(nc) + s;
				off[run] = k * DIR_ENT_SIZE;
				run++;
			}
		last = nc;
	}
	ret = 0;
out:
	kfree(it);
	return ret;
}

static int dir_add_entry(struct usb_device_data *data, u32 dir_cl,
			 const char *name, u8 attr, u32 cluster, u32 size)
{
	int nlfn = DIV_ROUND_UP((int)strlen(name), FAT_CHARS_PER_LFN);
	int n = nlfn + 1;
	u8 sfn[11];
	u8 *ents = NULL;
	u32 *lba = NULL;
	u16 *off = NULL;
	int ret;

	ret = make_sfn(data, dir_cl, name, sfn);
	if (ret)
		return ret;

	ents = kzalloc(n * DIR_ENT_SIZE, GFP_KERNEL);
	lba = kcalloc(n, sizeof(*lba), GFP_KERNEL);
	off = kcalloc(n, sizeof(*off), GFP_KERNEL);
	if (!ents || !lba || !off) {
		ret = -ENOMEM;
		goto out;
	}

	build_entries(name, sfn, attr, cluster, size, ents);

	ret = find_free_slots(data, dir_cl, n, lba, off);
	if (ret)
		goto out;
	ret = write_entries(data, lba, off, ents, n);
out:
	kfree(off);
	kfree(lba);
	kfree(ents);
	return ret;
}

/* Mark the SFN entry and its own LFN entries as deleted (0xE5). */
static int mark_deleted(struct usb_device_data *data,
			const struct fat_dirent *de)
{
	u32 lba[FAT_LFN_MAX + 1];
	u16 off[FAT_LFN_MAX + 1];
	u32 cur = U32_MAX;
	int i, n = 0, ret;

	for (i = 0; i < de->lfn_cnt; i++) {
		lba[n] = de->lfn_lba[i];
		off[n] = de->lfn_off[i];
		n++;
	}
	lba[n] = de->sfn_lba;
	off[n] = de->sfn_off;
	n++;

	for (i = 0; i < n; i++) {
		if (lba[i] != cur) {
			if (cur != U32_MAX) {
				ret = sec_write(data, cur, sec_buf);
				if (ret)
					return ret;
			}
			ret = sec_read(data, lba[i], sec_buf);
			if (ret)
				return ret;
			cur = lba[i];
		}
		sec_buf[off[i]] = DIRENT_DELETED;
	}
	return sec_write(data, cur, sec_buf);
}

/* Update first cluster, size and modified time of an existing entry. */
static int update_entry(struct usb_device_data *data,
			const struct fat_dirent *de, u32 cluster, u32 size)
{
	u16 t, d;
	u8 *e;
	int ret;

	ret = sec_read(data, de->sfn_lba, sec_buf);
	if (ret)
		return ret;

	fat_now(&t, &d);
	e = sec_buf + de->sfn_off;
	put_unaligned_le16(cluster >> 16, e + 20);
	put_unaligned_le16(cluster & 0xFFFF, e + 26);
	put_unaligned_le32(size, e + 28);
	put_unaligned_le16(t, e + 22);
	put_unaligned_le16(d, e + 24);
	put_unaligned_le16(d, e + 18);
	return sec_write(data, de->sfn_lba, sec_buf);
}

/* ------------------------------------------------------------------ */
/* File data                                                          */
/* ------------------------------------------------------------------ */

static int file_read(struct usb_device_data *data, u32 cl, u8 *buf, u32 len)
{
	u32 done = 0, s, chunk, next, hops = 0;
	int ret;

	while (done < len) {
		if (!clu_valid(cl))
			return -EIO;
		for (s = 0; s < bpb_instance.Sectors_per_Cluster && done < len; s++) {
			ret = sec_read(data, clu_lba(cl) + s, sec_buf);
			if (ret)
				return ret;
			chunk = min_t(u32, len - done, SECTOR_SIZE);
			memcpy(buf + done, sec_buf, chunk);
			done += chunk;
		}
		if (done >= len)
			break;
		ret = fat_get(data, cl, &next);
		if (ret)
			return ret;
		cl = next;
		if (++hops > bpb_instance.Max_Cluster)
			return -ELOOP;
	}
	return 0;
}

/* Write len bytes into a newly allocated chain. *first = 0 for len == 0. */
static int file_write(struct usb_device_data *data, const u8 *content,
		      u32 len, u32 *first)
{
	u32 nclu, i, s, chunk, cl, prev = 0, done = 0;
	int ret;

	*first = 0;
	if (len == 0)
		return 0;

	nclu = DIV_ROUND_UP(len, cluster_bytes());
	for (i = 0; i < nclu; i++) {
		ret = fat_alloc(data, &cl, false);
		if (ret)
			goto fail;
		if (prev) {
			ret = fat_set(data, prev, cl);
			if (ret) {
				fat_set(data, cl, 0);
				goto fail;
			}
		} else {
			*first = cl;
		}

		for (s = 0; s < bpb_instance.Sectors_per_Cluster && done < len; s++) {
			chunk = min_t(u32, len - done, SECTOR_SIZE);
			memset(sec_buf, 0, SECTOR_SIZE);
			memcpy(sec_buf, content + done, chunk);
			ret = sec_write(data, clu_lba(cl) + s, sec_buf);
			if (ret)
				goto fail;
			done += chunk;
		}
		prev = cl;
	}
	return 0;

fail:
	if (*first)
		fat_free_chain(data, *first);
	*first = 0;
	return ret;
}

/* ------------------------------------------------------------------ */
/* Paths                                                              */
/* ------------------------------------------------------------------ */

/* Next path component (NUL-terminated in place), or NULL at the end. */
static char *path_next(char **p)
{
	char *s = *p, *c;

	while (*s == '/')
		s++;
	if (!*s)
		return NULL;
	c = s;
	while (*s && *s != '/')
		s++;
	if (*s)
		*s++ = '\0';
	*p = s;
	return c;
}

static int resolve_dir(struct usb_device_data *data, char *path, u32 *out)
{
	struct fat_dirent *de;
	u32 cl = bpb_instance.Root_Cluster;
	char *comp;
	int ret = 0;

	de = kmalloc(sizeof(*de), GFP_KERNEL);
	if (!de)
		return -ENOMEM;

	while ((comp = path_next(&path)) != NULL) {
		if (!strcmp(comp, "."))
			continue;
		if (!strcmp(comp, "..")) {
			ret = -EINVAL;
			break;
		}
		ret = dir_lookup(data, cl, comp, de);
		if (ret)
			break;
		if (!(de->attr & ATTR_DIRECTORY)) {
			ret = -ENOTDIR;
			break;
		}
		if (!clu_valid(de->cluster)) {
			ret = -EIO;
			break;
		}
		cl = de->cluster;
	}

	kfree(de);
	if (!ret)
		*out = cl;
	return ret;
}

/* Split "a/b/c" into parent "a/b" and last "c", then resolve the parent. */
static int resolve_parent(struct usb_device_data *data, char *p,
			  u32 *pcl, char **last)
{
	size_t n = strlen(p);
	char *slash, *parent;

	while (n > 0 && p[n - 1] == '/')
		p[--n] = '\0';

	slash = strrchr(p, '/');
	if (!slash) {
		parent = p + n;              /* empty string -> root */
		*last = p;
	} else {
		*slash = '\0';
		parent = p;
		*last = slash + 1;
	}

	if (!**last || !strcmp(*last, ".") || !strcmp(*last, ".."))
		return -EINVAL;

	return resolve_dir(data, parent, pcl);
}

/* ------------------------------------------------------------------ */
/* Tree walk: delete and print                                        */
/* ------------------------------------------------------------------ */

static bool safe_child(u32 cl, const u32 *path, int depth)
{
	int i;

	if (!clu_valid(cl) || cl == bpb_instance.Root_Cluster)
		return false;
	for (i = 0; i <= depth; i++)
		if (path[i] == cl)
			return false;
	return true;
}

/* Free every chain below dir_cl (the directory's own chain is not freed). */
static int delete_tree(struct usb_device_data *data, u32 dir_cl, int depth,
		       u32 *path)
{
	struct fat_iter *it;
	struct fat_dirent *de;
	int ret;

	if (depth >= FAT_MAX_DEPTH) {
		printk(KERN_WARNING "FAT32: delete depth limit, subtree skipped\n");
		return 0;
	}
	path[depth] = dir_cl;

	it = iter_new(dir_cl);
	de = kmalloc(sizeof(*de), GFP_KERNEL);
	if (!it || !de) {
		ret = -ENOMEM;
		goto out;
	}

	while ((ret = iter_next(data, it, de)) > 0) {
		if (is_dot(de))
			continue;

		if (de->attr & ATTR_DIRECTORY) {
			if (!safe_child(de->cluster, path, depth)) {
				printk(KERN_WARNING
				       "FAT32: '%s' -> cluster %u loops/invalid, skipped\n",
				       de->name, de->cluster);
				continue;
			}
			ret = delete_tree(data, de->cluster, depth + 1, path);
			if (ret < 0)
				break;
		}
		if (clu_valid(de->cluster) &&
		    de->cluster != bpb_instance.Root_Cluster) {
			ret = fat_free_chain(data, de->cluster);
			if (ret < 0)
				break;
		}
	}
out:
	kfree(de);
	kfree(it);
	return ret < 0 ? ret : 0;
}

static void make_pad(char *buf, int level, u8 type)
{
	const char *m;
	int ind = level * 3;

	if (type == FILE_TYPE)
		m = "|----";
	else if (type == CONTENT_TYPE)
		m = "|-[.]";
	else
		m = "|-[ ]";

	if (ind > PAD_BUF_SIZE - 6)
		ind = PAD_BUF_SIZE - 6;
	memset(buf, ' ', ind);
	memcpy(buf + ind, m, 5);
	buf[ind + 5] = '\0';
}

static void print_content(struct usb_device_data *data,
			  const struct fat_dirent *de, int level)
{
	char pad[PAD_BUF_SIZE];
	u32 n = min_t(u32, de->size, FAT_PRINT_MAX);
	u8 *buf;

	if (n == 0)
		return;

	buf = kvmalloc(n + 1, GFP_KERNEL);
	if (!buf)
		return;

	if (file_read(data, de->cluster, buf, n) == 0) {
		buf[n] = '\0';
		make_pad(pad, level, CONTENT_TYPE);
		printk("%s '%s'%s\n", pad, buf, de->size > n ? " ..." : "");
	}
	kvfree(buf);
}

static int print_tree(struct usb_device_data *data, u32 dir_cl, int level,
		      int depth, u32 *path, bool recursive)
{
	char pad[PAD_BUF_SIZE];
	struct fat_iter *it;
	struct fat_dirent *de;
	int ret;

	path[depth] = dir_cl;

	it = iter_new(dir_cl);
	de = kmalloc(sizeof(*de), GFP_KERNEL);
	if (!it || !de) {
		ret = -ENOMEM;
		goto out;
	}

	while ((ret = iter_next(data, it, de)) > 0) {
		if (is_dot(de))
			continue;
#if (!HIDDEN_FOLDERS)
		if (de->name[0] == '.')
			continue;
#endif
		if (de->attr & ATTR_DIRECTORY) {
			make_pad(pad, level, DIR_TYPE);
			printk("%s '%s'\n", pad, de->name);

			if (!recursive)
				continue;
			if (!safe_child(de->cluster, path, depth)) {
				printk(KERN_WARNING
				       "FAT32: '%s' -> cluster %u loops/invalid, not entered\n",
				       de->name, de->cluster);
				continue;
			}
			if (depth + 1 >= FAT_MAX_DEPTH) {
				printk(KERN_WARNING "FAT32: depth limit at '%s'\n",
				       de->name);
				continue;
			}
			ret = print_tree(data, de->cluster, level + 1, depth + 1,
					 path, true);
			if (ret < 0)
				break;
		} else {
			make_pad(pad, level, FILE_TYPE);
			printk("%s '%s'\n", pad, de->name);
#if (PRINT_CONTENT)
			print_content(data, de, level + 1);
#endif
		}
	}
out:
	kfree(de);
	kfree(it);
	return ret < 0 ? ret : 0;
}

/* ------------------------------------------------------------------ */
/* Operations (called with fat_mutex held)                            */
/* ------------------------------------------------------------------ */

static int do_make_dir(struct usb_device_data *data, const u8 *path)
{
	struct fat_dirent *de = NULL;
	char *p, *last;
	u32 pcl, ncl;
	int ret;

	p = kstrndup(path, MAX_PATH_LEN, GFP_KERNEL);
	de = kmalloc(sizeof(*de), GFP_KERNEL);
	if (!p || !de) {
		ret = -ENOMEM;
		goto out;
	}

	ret = resolve_parent(data, p, &pcl, &last);
	if (ret)
		goto out;
	ret = valid_name(last);
	if (ret)
		goto out;

	/* Check BEFORE allocating, so repeated runs don't leak clusters */
	ret = dir_lookup(data, pcl, last, de);
	if (ret == 0) {
		if (de->attr & ATTR_DIRECTORY) {
			fat_dbg("Dir already existed: %s\n", last);
			ret = 0;
		} else {
			ret = -EEXIST;
		}
		goto out;
	}
	if (ret != -ENOENT)
		goto out;

	ret = fat_alloc(data, &ncl, true);       /* zeroed directory cluster */
	if (ret)
		goto out;

	/* "." and ".." (".." is 0 when the parent is the root, per spec) */
	memset(sec_buf, 0, SECTOR_SIZE);
	fill_sfn(sec_buf, (const u8 *)".          ", ATTR_DIRECTORY, ncl, 0);
	fill_sfn(sec_buf + DIR_ENT_SIZE, (const u8 *)"..         ",
		 ATTR_DIRECTORY,
		 pcl == bpb_instance.Root_Cluster ? 0 : pcl, 0);
	ret = sec_write(data, clu_lba(ncl), sec_buf);
	if (ret) {
		fat_free_chain(data, ncl);
		goto out;
	}

	ret = dir_add_entry(data, pcl, last, ATTR_DIRECTORY, ncl, 0);
	if (ret)
		fat_free_chain(data, ncl);
out:
	kfree(de);
	kfree(p);
	return ret;
}

static int do_make_file(struct usb_device_data *data, const u8 *path)
{
	struct fat_dirent *de = NULL;
	char *p, *last;
	u32 pcl;
	int ret;

	p = kstrndup(path, MAX_PATH_LEN, GFP_KERNEL);
	de = kmalloc(sizeof(*de), GFP_KERNEL);
	if (!p || !de) {
		ret = -ENOMEM;
		goto out;
	}

	ret = resolve_parent(data, p, &pcl, &last);
	if (ret)
		goto out;
	ret = valid_name(last);
	if (ret)
		goto out;

	ret = dir_lookup(data, pcl, last, de);
	if (ret == 0) {
		if (!(de->attr & ATTR_DIRECTORY)) {
			fat_dbg("File already existed: %s\n", last);
			ret = 0;
		} else {
			ret = -EEXIST;
		}
		goto out;
	}
	if (ret != -ENOENT)
		goto out;

	/* Empty file: first cluster 0, size 0 (valid FAT32) */
	ret = dir_add_entry(data, pcl, last, ATTR_ARCHIVE, 0, 0);
out:
	kfree(de);
	kfree(p);
	return ret;
}

/* Look up path; want_dir selects directory or file. */
static int lookup_path(struct usb_device_data *data, const u8 *path,
		       struct fat_dirent *de, bool want_dir)
{
	char *p, *last;
	u32 pcl;
	int ret;

	p = kstrndup(path, MAX_PATH_LEN, GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	ret = resolve_parent(data, p, &pcl, &last);
	if (!ret)
		ret = dir_lookup(data, pcl, last, de);
	if (!ret) {
		if (want_dir && !(de->attr & ATTR_DIRECTORY))
			ret = -ENOTDIR;
		else if (!want_dir && (de->attr & ATTR_DIRECTORY))
			ret = -EISDIR;
	}
	kfree(p);
	return ret;
}

static int do_remove_file(struct usb_device_data *data, const u8 *path)
{
	struct fat_dirent *de = kmalloc(sizeof(*de), GFP_KERNEL);
	int ret;

	if (!de)
		return -ENOMEM;

	ret = lookup_path(data, path, de, false);
	if (ret == -ENOENT)
		fat_dbg("File not existed: %s\n", path);
	if (ret)
		goto out;

	ret = mark_deleted(data, de);              /* entry first ...     */
	if (!ret && clu_valid(de->cluster))
		ret = fat_free_chain(data, de->cluster); /* ... then its data */
out:
	kfree(de);
	return ret;
}

static int do_remove_dir(struct usb_device_data *data, const u8 *path)
{
	struct fat_dirent *de = kmalloc(sizeof(*de), GFP_KERNEL);
	u32 tree_path[FAT_MAX_DEPTH];
	int ret;

	if (!de)
		return -ENOMEM;

	ret = lookup_path(data, path, de, true);
	if (ret == -ENOENT)
		fat_dbg("Dir not existed: %s\n", path);
	if (ret)
		goto out;

	/* Recursive: free everything inside, then the entry, then the dir */
	if (clu_valid(de->cluster) &&
	    de->cluster != bpb_instance.Root_Cluster) {
		ret = delete_tree(data, de->cluster, 0, tree_path);
		if (ret)
			goto out;
	}
	ret = mark_deleted(data, de);
	if (!ret && clu_valid(de->cluster))
		ret = fat_free_chain(data, de->cluster);
out:
	kfree(de);
	return ret;
}

static int do_add_content(struct usb_device_data *data, const u8 *path,
			  const u8 *content)
{
	struct fat_dirent *de = kmalloc(sizeof(*de), GFP_KERNEL);
	u32 len, ncl, old;
	int ret;

	if (!de)
		return -ENOMEM;

	ret = lookup_path(data, path, de, false);
	if (ret) {
		printk(KERN_ERR "FAT32: Add_Content: invalid file '%s' (%d)\n",
		       path, ret);
		goto out;
	}

	len = strnlen(content, FAT_CONTENT_MAX);
	old = de->cluster;
	if (de->size)
		fat_dbg("Data will be overwritten\n");

	/* New data first, then switch the entry, then free the old chain */
	ret = file_write(data, content, len, &ncl);
	if (ret)
		goto out;
	ret = update_entry(data, de, ncl, len);
	if (ret) {
		if (ncl)
			fat_free_chain(data, ncl);
		goto out;
	}
	if (clu_valid(old) && old != ncl)
		ret = fat_free_chain(data, old);
out:
	kfree(de);
	return ret;
}

static int do_delete_content(struct usb_device_data *data, const u8 *path)
{
	struct fat_dirent *de = kmalloc(sizeof(*de), GFP_KERNEL);
	int ret;

	if (!de)
		return -ENOMEM;

	ret = lookup_path(data, path, de, false);
	if (ret) {
		printk(KERN_ERR "FAT32: Delete_Content: invalid file '%s' (%d)\n",
		       path, ret);
		goto out;
	}

	ret = update_entry(data, de, 0, 0);
	if (!ret && clu_valid(de->cluster))
		ret = fat_free_chain(data, de->cluster);
out:
	kfree(de);
	return ret;
}

static int do_read_file(struct usb_device_data *data, const u8 *path)
{
	struct fat_dirent *de = kmalloc(sizeof(*de), GFP_KERNEL);
	u32 n;
	u8 *buf = NULL;
	int ret;

	if (!de)
		return -ENOMEM;

	ret = lookup_path(data, path, de, false);
	if (ret) {
		printk("Invalid file: %s\n", path);
		goto out;
	}

	n = min_t(u32, de->size, FAT_PRINT_MAX);
	buf = kvmalloc(n + 1, GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto out;
	}
	if (n) {
		ret = file_read(data, de->cluster, buf, n);
		if (ret)
			goto out;
	}
	buf[n] = '\0';

	printk("%s\n", path);
	printk("-> '%s'%s\n", buf, de->size > n ? " ..." : "");
out:
	kvfree(buf);
	kfree(de);
	return ret;
}

static int do_read_dir(struct usb_device_data *data, const u8 *path)
{
	u32 tree_path[FAT_MAX_DEPTH];
	char *p;
	u32 cl;
	int ret;

	p = kstrndup(path, MAX_PATH_LEN, GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	ret = resolve_dir(data, p, &cl);
	if (ret) {
		printk("Invalid dir: %s\n", path);
		goto out;
	}

	printk("'%s':\n", path);
	ret = print_tree(data, cl, 1, 0, tree_path, false);
out:
	kfree(p);
	return ret;
}

static int do_clear_all(struct usb_device_data *data)
{
	u32 tree_path[FAT_MAX_DEPTH];
	u32 root = bpb_instance.Root_Cluster;
	u32 next;
	int ret;

	/* 1. Free every chain reachable from the root */
	ret = delete_tree(data, root, 0, tree_path);
	if (ret)
		return ret;

	/* 2. Shrink the root directory to one cluster */
	ret = fat_get(data, root, &next);
	if (ret)
		return ret;
	ret = fat_set(data, root, FAT32_EOC);
	if (ret)
		return ret;
	if (clu_valid(next) && next != root) {
		ret = fat_free_chain(data, next);
		if (ret)
			return ret;
	}

	/* 3. Empty root directory (also removes the volume label entry) */
	ret = zero_cluster(data, root);
	if (ret)
		return ret;

	alloc_hint = 3;
	printk(KERN_INFO "FAT32: volume cleared\n");
	return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

static int fat_begin(struct usb_device_data *data)
{
	if (!data)
		return -EINVAL;
	mutex_lock(&fat_mutex);
	if (!data->done_mounted) {
		mutex_unlock(&fat_mutex);
		printk(KERN_ERR "FAT32: not mounted\n");
		return -ENODEV;
	}
	return 0;
}

static inline void fat_end(void)
{
	mutex_unlock(&fat_mutex);
}

int USB1_f_Mount(struct usb_device_data *data)
{
	u32 part_lba = 0, part_secs = 0, entry, start, count, fat_entries;
	u16 bps;
	u8 spc, type = 0;
	bool superfloppy;
	int i, ret;

	if (!data)
		return -EINVAL;

	mutex_lock(&fat_mutex);
	if (data->done_mounted) {
		ret = 0;
		goto out;
	}

	/* Reset driver state for a new volume */
	fat_cache_lba = U32_MAX;
	alloc_hint = 3;
	fsinfo_invalidated = false;

	/* LBA 0: MBR, or a VBR on a superfloppy */
	ret = sec_read(data, 0, sec_buf);
	if (ret)
		goto out;

	if (sec_buf[510] != 0x55 || sec_buf[511] != 0xAA) {
		printk(KERN_ERR "FAT32: no 55 AA signature at LBA 0\n");
		ret = -EINVAL;
		goto out;
	}

	bps = get_unaligned_le16(sec_buf + 11);
	spc = sec_buf[13];
	superfloppy = (sec_buf[0] == 0xEB || sec_buf[0] == 0xE9) &&
		      bps == SECTOR_SIZE && spc && !(spc & (spc - 1)) &&
		      get_unaligned_le16(sec_buf + 14) != 0 &&
		      sec_buf[16] != 0 &&
		      get_unaligned_le16(sec_buf + 22) == 0 &&     /* FATSz16 */
		      get_unaligned_le32(sec_buf + 36) != 0;       /* FATSz32 */

	if (superfloppy) {
		part_lba = 0;
		type = 0x0C;
		printk("FAT32: boot sector directly at LBA 0 (no MBR)\n");
	} else {
		for (i = 0; i < 4; i++) {
			entry = 0x1BE + i * 16;
			type = sec_buf[entry + 4];
			start = get_unaligned_le32(sec_buf + entry + 8);
			count = get_unaligned_le32(sec_buf + entry + 12);
			if ((type == 0x0B || type == 0x0C) && start && count) {
				part_lba = start;
				part_secs = count;
				break;
			}
		}
		if (!part_lba) {
			printk(KERN_ERR "FAT32: no FAT32 partition (0x0B/0x0C)\n");
			ret = -EINVAL;
			goto out;
		}
		printk("MBR: FAT32 partition type=0x%02x, start LBA=%u, sectors=%u\n",
		       type, part_lba, part_secs);
		ret = sec_read(data, part_lba, sec_buf);
		if (ret)
			goto out;
		if (sec_buf[510] != 0x55 || sec_buf[511] != 0xAA) {
			printk(KERN_ERR "FAT32: invalid boot sector signature\n");
			ret = -EINVAL;
			goto out;
		}
	}

	memset(&bpb_instance, 0, sizeof(bpb_instance));
	bpb_instance.Starting_LBA = part_lba;
	bpb_instance.Partition_type = type;
	bpb_instance.Bytes_per_Sector = get_unaligned_le16(sec_buf + 11);
	bpb_instance.Sectors_per_Cluster = sec_buf[13];
	bpb_instance.Reserved_Sectors = get_unaligned_le16(sec_buf + 14);
	bpb_instance.Number_of_FATs = sec_buf[16];
	bpb_instance.Total_Sectors = get_unaligned_le16(sec_buf + 19);
	if (!bpb_instance.Total_Sectors)
		bpb_instance.Total_Sectors = get_unaligned_le32(sec_buf + 32);
	bpb_instance.Sectors_per_FAT = get_unaligned_le32(sec_buf + 36);
	bpb_instance.Root_Cluster = get_unaligned_le32(sec_buf + 44);
	bpb_instance.FSInfo_Sector = get_unaligned_le16(sec_buf + 48);
	memcpy(bpb_instance.OME_name, sec_buf + 3, 8);
	memcpy(bpb_instance.FS_type, sec_buf + 82, 8);

	spc = bpb_instance.Sectors_per_Cluster;
	if (bpb_instance.Bytes_per_Sector != SECTOR_SIZE || !spc ||
	    (spc & (spc - 1)) || !bpb_instance.Reserved_Sectors ||
	    !bpb_instance.Number_of_FATs || !bpb_instance.Sectors_per_FAT ||
	    get_unaligned_le16(sec_buf + 22) != 0) {
		printk(KERN_ERR "FAT32: invalid/unsupported BPB\n");
		ret = -EINVAL;
		goto out;
	}

	bpb_instance.Data_Sector = bpb_instance.Reserved_Sectors +
		bpb_instance.Number_of_FATs * bpb_instance.Sectors_per_FAT;
	if (bpb_instance.Total_Sectors <= bpb_instance.Data_Sector) {
		printk(KERN_ERR "FAT32: total sectors too small\n");
		ret = -EINVAL;
		goto out;
	}

	bpb_instance.Cluster_Count =
		(bpb_instance.Total_Sectors - bpb_instance.Data_Sector) / spc;
	bpb_instance.Max_Cluster = bpb_instance.Cluster_Count + 1;

	/* Never address beyond the FAT itself */
	fat_entries = bpb_instance.Sectors_per_FAT * FAT_ENTS_PER_SEC;
	if (bpb_instance.Max_Cluster >= fat_entries)
		bpb_instance.Max_Cluster = fat_entries - 1;

	if (!clu_valid(bpb_instance.Root_Cluster)) {
		printk(KERN_ERR "FAT32: invalid root cluster %u\n",
		       bpb_instance.Root_Cluster);
		ret = -EINVAL;
		goto out;
	}
	if (bpb_instance.Cluster_Count < 65525)
		printk(KERN_WARNING "FAT32: only %u clusters, volume may be FAT16\n",
		       bpb_instance.Cluster_Count);

	printk("FAT32 mounted: LBA=%u, sec/clu=%u (%u B), reserved=%u, FATs=%u, "
	       "sec/FAT=%u, root=%u, clusters=%u\n",
	       bpb_instance.Starting_LBA, spc, cluster_bytes(),
	       bpb_instance.Reserved_Sectors, bpb_instance.Number_of_FATs,
	       bpb_instance.Sectors_per_FAT, bpb_instance.Root_Cluster,
	       bpb_instance.Cluster_Count);

	data->done_mounted = true;
	ret = 0;
out:
	mutex_unlock(&fat_mutex);
	return ret;
}

int USB1_f_Read_All(struct usb_device_data *data)
{
	u32 tree_path[FAT_MAX_DEPTH];
	int ret = fat_begin(data);

	if (ret)
		return ret;
	printk("================= [Files and Folders] ==================\n");
	ret = print_tree(data, bpb_instance.Root_Cluster, 0, 0, tree_path, true);
	fat_end();
	return ret;
}

int USB1_f_Read_Dir(struct usb_device_data *data, u8 *path_dir)
{
	int ret = fat_begin(data);

	if (ret)
		return ret;
	ret = do_read_dir(data, path_dir);
	fat_end();
	return ret;
}

int USB1_f_Read_File(struct usb_device_data *data, u8 *path_file)
{
	int ret = fat_begin(data);

	if (ret)
		return ret;
	ret = do_read_file(data, path_file);
	fat_end();
	return ret;
}

int USB1_f_Make_Dir(struct usb_device_data *data, u8 *path_dir)
{
	int ret = fat_begin(data);

	if (ret)
		return ret;
	ret = do_make_dir(data, path_dir);
	fat_end();
	if (ret)
		printk(KERN_ERR "FAT32: Make_Dir '%s' failed (%d)\n", path_dir, ret);
	return ret;
}

int USB1_f_Remove_Dir(struct usb_device_data *data, u8 *path_dir)
{
	int ret = fat_begin(data);

	if (ret)
		return ret;
	ret = do_remove_dir(data, path_dir);
	fat_end();
	if (ret)
		printk(KERN_ERR "FAT32: Remove_Dir '%s' failed (%d)\n", path_dir, ret);
	return ret;
}

int USB1_f_Make_File(struct usb_device_data *data, u8 *path_file)
{
	int ret = fat_begin(data);

	if (ret)
		return ret;
	ret = do_make_file(data, path_file);
	fat_end();
	if (ret)
		printk(KERN_ERR "FAT32: Make_File '%s' failed (%d)\n", path_file, ret);
	return ret;
}

int USB1_f_Remove_File(struct usb_device_data *data, u8 *path_file)
{
	int ret = fat_begin(data);

	if (ret)
		return ret;
	ret = do_remove_file(data, path_file);
	fat_end();
	if (ret)
		printk(KERN_ERR "FAT32: Remove_File '%s' failed (%d)\n", path_file, ret);
	return ret;
}

int USB1_f_Add_Content(struct usb_device_data *data, u8 *path_file, u8 *content)
{
	int ret;

	if (!content)
		return -EINVAL;
	ret = fat_begin(data);
	if (ret)
		return ret;
	ret = do_add_content(data, path_file, content);
	fat_end();
	return ret;
}

int USB1_f_Delete_Content(struct usb_device_data *data, u8 *path_file)
{
	int ret = fat_begin(data);

	if (ret)
		return ret;
	ret = do_delete_content(data, path_file);
	fat_end();
	return ret;
}

/*
 * Delete all files and directories on the mounted volume.
 * Frees only the chains reachable from the root (fast: proportional to the
 * content, not to the FAT size). Orphaned clusters from an earlier corrupt
 * state are NOT recovered; run fsck.vfat / chkdsk once for that.
 */
int USB1_f_Clear_All(struct usb_device_data *data)
{
	int ret = fat_begin(data);

	if (ret)
		return ret;
	ret = do_clear_all(data);
	fat_end();
	return ret;
}