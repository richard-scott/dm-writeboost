/*
 * Copyright (C) 2012-2013 Akira Hayakawa <ruby.wktk@gmail.com>
 *
 * This file is released under the GPL.
 */

#ifndef DM_WRITEBOOST_H
#define DM_WRITEBOOST_H

#define DM_MSG_PREFIX "writeboost"

#include <linux/module.h>
#include <linux/version.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/device-mapper.h>
#include <linux/dm-io.h>

#define WBERR(f, args...) \
	DMERR("err@%d " f, __LINE__, ## args)
#define WBWARN(f, args...) \
	DMWARN("warn@%d " f, __LINE__, ## args)
#define WBINFO(f, args...) \
	DMINFO("info@%d " f, __LINE__, ## args)


/*
 * (1 << x) sector.
 * 4 <= x <= 11
 * dm-writeboost supports segment size up to 1MB.
 *
 * All the comments are if
 * the segment size is the maximum 1MB.
 */
#define WB_SEGMENTSIZE_ORDER 11

/*
 * By default,
 * we allocate 64 * 1MB RAM buffers statically.
 */
#define NR_RAMBUF_POOL 64

/*
 * The first 4KB (1<<3 sectors) in segment
 * is for metadata.
 */
#define NR_CACHES_INSEG ((1 << (WB_SEGMENTSIZE_ORDER - 3)) - 1)

/*
 * The Detail of the Disk Format
 *
 * Whole:
 * Superblock(1MB) Segment(1MB) Segment(1MB) ...
 * We reserve the first segment (1MB) as the superblock.
 *
 * Superblock(1MB):
 * head <----                               ----> tail
 * superblock header(512B) ... superblock record(512B)
 *
 * Segment(1MB):
 * segment_header_device(4KB) metablock_device(4KB) * NR_CACHES_INSEG
 */

/*
 * Superblock Header
 * First one sector of the super block region.
 * The value is fixed after formatted.
 */

 /*
  * Magic Number
  * "WBst"
  */
#define WRITEBOOST_MAGIC 0x57427374
struct superblock_header_device {
	__le32 magic;
} __packed;

/*
 * Superblock Record (Mutable)
 * Last one sector of the superblock region.
 * Record the current cache status in need.
 */
struct superblock_record_device {
	__le64 last_migrated_segment_id;
} __packed;

/*
 * Cache line index.
 *
 * dm-writeboost can supoort a cache device
 * with size less than 4KB * (1 << 32)
 * that is 16TB.
 */
typedef u32 cache_nr;

/*
 * Metadata of a 4KB cache line
 *
 * Dirtiness is defined for each sector
 * in this cache line.
 */
struct metablock {
	sector_t sector; /* key */

	cache_nr idx; /* Const */

	struct hlist_node ht_list;

	/*
	 * 8 bit flag for dirtiness
	 * for each sector in cache line.
	 *
	 * Current implementation
	 * only recovers dirty caches.
	 * Recovering clean caches complicates the code
	 * but couldn't be effective
	 * since only few of the caches are clean.
	 */
	u8 dirty_bits;
};

/*
 * On-disk metablock
 */
struct metablock_device {
	__le64 sector;

	u8 dirty_bits;

	__le32 lap;
} __packed;

#define SZ_MAX (~(size_t)0)
struct segment_header {
	struct metablock mb_array[NR_CACHES_INSEG];

	/*
	 * ID uniformly increases.
	 * ID 0 is used to tell that the segment is invalid
	 * and valid id >= 1.
	 */
	u64 global_id;

	/*
	 * Segment can be flushed half-done.
	 * length is the number of
	 * metablocks that must be counted in
	 * in resuming.
	 */
	u8 length;

	cache_nr start_idx; /* Const */
	sector_t start_sector; /* Const */

	struct list_head migrate_list;

	/*
	 * This segment can not be migrated
	 * to backin store
	 * until flushed.
	 * Flushed segment is in cache device.
	 */
	struct completion flush_done;

	/*
	 * This segment can not be overwritten
	 * until migrated.
	 */
	struct completion migrate_done;

	spinlock_t lock;

	atomic_t nr_inflight_ios;
};

/*
 * (Locking)
 * Locking metablocks by their granularity
 * needs too much memory space for lock structures.
 * We only locks a metablock by locking the parent segment
 * that includes the metablock.
 */
#define lockseg(seg, flags) spin_lock_irqsave(&(seg)->lock, flags)
#define unlockseg(seg, flags) spin_unlock_irqrestore(&(seg)->lock, flags)

/*
 * On-disk segment header.
 *
 * Must be at most 4KB large.
 */
struct segment_header_device {
	/* - FROM - At most512 byte for atomicity. --- */
	__le64 global_id;
	/*
	 * How many cache lines in this segments
	 * should be counted in resuming.
	 */
	u8 length;
	/*
	 * On what lap in rorating on cache device
	 * used to find the head and tail in the
	 * segments in cache device.
	 */
	__le32 lap;
	/* - TO -------------------------------------- */
	/* This array must locate at the tail */
	struct metablock_device mbarr[NR_CACHES_INSEG];
} __packed;

struct rambuffer {
	void *data;
	struct completion done;
};

enum STATFLAG {
	STAT_WRITE = 0,
	STAT_HIT,
	STAT_ON_BUFFER,
	STAT_FULLSIZE,
};
#define STATLEN (1 << 4)

struct lookup_key {
	sector_t sector;
};

struct ht_head {
	struct hlist_head ht_list;
};

struct wb_device;
struct wb_cache {
	struct wb_device *wb;

	struct dm_dev *device;
	struct mutex io_lock;
	cache_nr nr_caches; /* Const */
	u64 nr_segments; /* Const */
	struct bigarray *segment_header_array;

	/*
	 * Chained hashtable
	 *
	 * Writeboost uses chained hashtable
	 * to cache lookup.
	 * Cache discarding often happedns
	 * This structure fits our needs.
	 */
	struct bigarray *htable;
	size_t htsize;
	struct ht_head *null_head;

	cache_nr cursor; /* Index that has been written the most lately */
	struct segment_header *current_seg;
	struct rambuffer *current_rambuf;
	struct rambuffer *rambuf_pool;

	u64 last_migrated_segment_id;
	u64 last_flushed_segment_id;
	u64 reserving_segment_id;

	/*
	 * Flush daemon
	 *
	 * Writeboost first queue the segment to flush
	 * and flush daemon asynchronously
	 * flush them to the cache device.
	 */
	struct work_struct flush_work;
	struct workqueue_struct *flush_wq;
	spinlock_t flush_queue_lock;
	struct list_head flush_queue;
	wait_queue_head_t flush_wait_queue;

	/*
	 * Deferred ACK for barriers.
	 */
	struct work_struct barrier_deadline_work;
	struct timer_list barrier_deadline_timer;
	struct bio_list barrier_ios;
	unsigned long barrier_deadline_ms; /* param */

	/*
	 * Migration daemon
	 *
	 * Migartion also works in background.
	 *
	 * If allow_migrate is true,
	 * migrate daemon goes into migration
	 * if they are segments to migrate.
	 */
	struct work_struct migrate_work;
	struct workqueue_struct *migrate_wq;
	bool allow_migrate; /* param */

	/*
	 * Batched Migration
	 *
	 * Migration is done atomically
	 * with number of segments batched.
	 */
	wait_queue_head_t migrate_wait_queue;
	atomic_t migrate_fail_count;
	atomic_t migrate_io_count;
	struct list_head migrate_list;
	u8 *dirtiness_snapshot;
	void *migrate_buffer;
	size_t nr_cur_batched_migration;
	size_t nr_max_batched_migration; /* param */

	/*
	 * Migration modulator
	 *
	 * This daemon turns on and off
	 * the migration
	 * according to the load of backing store.
	 */
	struct work_struct modulator_work;
	bool enable_migration_modulator; /* param */

	/*
	 * Superblock Recorder
	 *
	 * Update the superblock record
	 * periodically.
	 */
	struct work_struct recorder_work;
	unsigned long update_record_interval; /* param */

	/*
	 * Cache Synchronizer
	 *
	 * Sync the dirty writes
	 * periodically.
	 */
	struct work_struct sync_work;
	unsigned long sync_interval; /* param */

	/*
	 * on_terminate is true
	 * to notify all the background daemons to
	 * stop their operations.
	 */
	bool on_terminate;

	atomic64_t stat[STATLEN];
};

struct wb_device {
	struct dm_target *ti;

	struct dm_dev *device;

	struct wb_cache *cache;

	u8 migrate_threshold;

	atomic64_t nr_dirty_caches;
};

struct flush_job {
	struct list_head flush_queue;
	struct segment_header *seg;
	/*
	 * The data to flush to cache device.
	 */
	struct rambuffer *rambuf;
	/*
	 * List of bios with barrier flags.
	 */
	struct bio_list barrier_ios;
};

#define PER_BIO_VERSION KERNEL_VERSION(3, 8, 0)
#if LINUX_VERSION_CODE >= PER_BIO_VERSION
struct per_bio_data {
	void *ptr;
};
#endif
#endif