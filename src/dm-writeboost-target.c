/*
 * dm-writeboost
 * Log-structured Caching for Linux
 *
 * This file is part of dm-writeboost
 * Copyright (C) 2012-2016 Akira Hayakawa <ruby.wktk@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "dm-writeboost.h"
#include "dm-writeboost-metadata.h"
#include "dm-writeboost-daemon.h"

#include "linux/sort.h"

/*----------------------------------------------------------------------------*/

void bio_endio_compat(struct bio *bio, int error)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,3,0)
	bio->bi_error = error;
	bio_endio(bio);
#else
	bio_endio(bio, error);
#endif
}

/*----------------------------------------------------------------------------*/

void do_check_buffer_alignment(void *buf, const char *name, const char *caller)
{
	unsigned long addr = (unsigned long) buf;

	if (!IS_ALIGNED(addr, 1 << 9)) {
		DMCRIT("@%s in %s is not sector-aligned. I/O buffer must be sector-aligned.", name, caller);
		BUG();
	}
}

struct wb_io {
	struct work_struct work;
	int err;
	unsigned long err_bits;
	struct dm_io_request *io_req;
	unsigned num_regions;
	struct dm_io_region *regions;
};

static void wb_io_fn(struct work_struct *work)
{
	struct wb_io *io = container_of(work, struct wb_io, work);
	io->err_bits = 0;
	io->err = dm_io(io->io_req, io->num_regions, io->regions, &io->err_bits);
}

int wb_io_internal(struct wb_device *wb, struct dm_io_request *io_req,
		   unsigned num_regions, struct dm_io_region *regions,
		   unsigned long *err_bits, bool thread, const char *caller)
{
	int err = 0;

	if (thread) {
		struct wb_io io = {
			.io_req = io_req,
			.regions = regions,
			.num_regions = num_regions,
		};
		BUG_ON(io_req->notify.fn);

		INIT_WORK_ONSTACK(&io.work, wb_io_fn);
		queue_work(wb->io_wq, &io.work);
		flush_workqueue(wb->io_wq);
		destroy_work_on_stack(&io.work); /* Pair with INIT_WORK_ONSTACK */

		err = io.err;
		if (err_bits)
			*err_bits = io.err_bits;
	} else {
		err = dm_io(io_req, num_regions, regions, err_bits);
	}

	/*
	 * err_bits can be NULL.
	 */
	if (err || (err_bits && *err_bits)) {
		char buf[BDEVNAME_SIZE];
		dev_t dev = regions->bdev->bd_dev;

		unsigned long eb;
		if (!err_bits)
			eb = (~(unsigned long)0);
		else
			eb = *err_bits;

		format_dev_t(buf, dev);
		DMERR("%s() I/O error(%d), bits(%lu), dev(%s), sector(%llu), rw(%d)",
		      caller, err, eb,
		      buf, (unsigned long long) regions->sector, io_req->bi_rw);
	}

	return err;
}

sector_t dm_devsize(struct dm_dev *dev)
{
	return i_size_read(dev->bdev->bd_inode) >> 9;
}

/*----------------------------------------------------------------------------*/

#if LINUX_VERSION_CODE <= KERNEL_VERSION(3,14,0)
#define bi_sector(bio) (bio)->bi_sector
#define bi_size(bio) (bio)->bi_size
#else
#define bi_sector(bio) (bio)->bi_iter.bi_sector
#define bi_size(bio) (bio)->bi_iter.bi_size
#endif

static void bio_remap(struct bio *bio, struct dm_dev *dev, sector_t sector)
{
	bio->bi_bdev = dev->bdev;
	bi_sector(bio) = sector;
}

static u8 do_io_offset(sector_t sector)
{
	u32 tmp32;
	div_u64_rem(sector, 1 << 3, &tmp32);
	return tmp32;
}

static u8 io_offset(struct bio *bio)
{
	return do_io_offset(bi_sector(bio));
}

static bool io_fullsize(struct bio *bio)
{
	return bio_sectors(bio) == (1 << 3);
}

static bool io_write(struct bio *bio)
{
	return bio_data_dir(bio) == WRITE;
}

/*
 * We use 4KB alignment address of original request the as the lookup key.
 */
static sector_t calc_cache_alignment(sector_t bio_sector)
{
	return div_u64(bio_sector, 1 << 3) * (1 << 3);
}

/*----------------------------------------------------------------------------*/

/*
 * Wake up the processes on the wq if the wq is active.
 * (At least a process is waiting on it)
 * This function should only used for wq that is rarely active.
 * Otherwise ordinary wake_up() should be used instead.
 */
static void wake_up_active_wq(wait_queue_head_t *wq)
{
	if (unlikely(waitqueue_active(wq)))
		wake_up(wq);
}

/*----------------------------------------------------------------------------*/

static u8 count_dirty_caches_remained(struct segment_header *seg)
{
	u8 i, count = 0;
	struct metablock *mb;
	for (i = 0; i < seg->length; i++) {
		mb = seg->mb_array + i;
		if (mb->dirtiness.is_dirty)
			count++;
	}
	return count;
}

/*
 * Prepare the RAM buffer for segment write.
 */
static void prepare_rambuffer(struct rambuffer *rambuf,
			      struct wb_device *wb,
			      struct segment_header *seg)
{
	prepare_segment_header_device(rambuf->data, wb, seg);
}

static void init_rambuffer(struct wb_device *wb)
{
	memset(wb->current_rambuf->data, 0, 1 << 12);
}

/*
 * Acquire a new RAM buffer for the new segment.
 */
static void __acquire_new_rambuffer(struct wb_device *wb, u64 id)
{
	struct rambuffer *next_rambuf;
	u32 tmp32;

	wait_for_flushing(wb, SUB_ID(id, NR_RAMBUF_POOL));

	div_u64_rem(id - 1, NR_RAMBUF_POOL, &tmp32);
	next_rambuf = wb->rambuf_pool + tmp32;

	wb->current_rambuf = next_rambuf;

	init_rambuffer(wb);
}

static void __acquire_new_seg(struct wb_device *wb, u64 id)
{
	struct segment_header *new_seg = get_segment_header_by_id(wb, id);

	/*
	 * We wait for all requests to the new segment is consumed.
	 * Mutex taken guarantees that no new I/O to this segment is coming in.
	 */
	wait_event(wb->inflight_ios_wq,
		!atomic_read(&new_seg->nr_inflight_ios));

	wait_for_writeback(wb, SUB_ID(id, wb->nr_segments));
	if (count_dirty_caches_remained(new_seg)) {
		DMERR("%u dirty caches remained. id:%llu",
		      count_dirty_caches_remained(new_seg), id);
		BUG();
	}
	discard_caches_inseg(wb, new_seg);

	/*
	 * We mustn't set new id to the new segment before
	 * all wait_* events are done since they uses those id for waiting.
	 */
	new_seg->id = id;
	wb->current_seg = new_seg;
}

/*
 * Acquire the new segment and RAM buffer for the following writes.
 * Guarantees all dirty caches in the segments are written back and
 * all metablocks in it are invalidated (Linked to null head).
 */
void acquire_new_seg(struct wb_device *wb, u64 id)
{
	__acquire_new_rambuffer(wb, id);
	__acquire_new_seg(wb, id);
}

static void prepare_new_seg(struct wb_device *wb)
{
	u64 next_id = wb->current_seg->id + 1;
	acquire_new_seg(wb, next_id);
	cursor_init(wb);
}

/*----------------------------------------------------------------------------*/

static void copy_barrier_requests(struct flush_job *job, struct wb_device *wb)
{
	bio_list_init(&job->barrier_ios);
	bio_list_merge(&job->barrier_ios, &wb->barrier_ios);
	bio_list_init(&wb->barrier_ios);
}

static void init_flush_job(struct flush_job *job, struct wb_device *wb)
{
	job->wb = wb;
	job->seg = wb->current_seg;

	copy_barrier_requests(job, wb);
}

static void queue_flush_job(struct wb_device *wb)
{
	struct flush_job *job = &wb->current_rambuf->job;

	wait_event(wb->inflight_ios_wq, !atomic_read(&wb->current_seg->nr_inflight_ios));

	prepare_rambuffer(wb->current_rambuf, wb, wb->current_seg);

	init_flush_job(job, wb);
	INIT_WORK(&job->work, flush_proc);
	queue_work(wb->flusher_wq, &job->work);
}

static void queue_current_buffer(struct wb_device *wb)
{
	queue_flush_job(wb);
	prepare_new_seg(wb);
}

void cursor_init(struct wb_device *wb)
{
	wb->cursor = wb->current_seg->start_idx;
	wb->current_seg->length = 0;
}

/*
 * Flush out all the transient data at a moment but _NOT_ persistently.
 */
void flush_current_buffer(struct wb_device *wb)
{
	struct segment_header *old_seg;

	mutex_lock(&wb->io_lock);
	old_seg = wb->current_seg;

	queue_current_buffer(wb);

	cursor_init(wb); /* FIXME this looks dup call */
	mutex_unlock(&wb->io_lock);

	wait_for_flushing(wb, old_seg->id);
}

/*----------------------------------------------------------------------------*/

static void inc_stat(struct wb_device *wb,
		     int rw, bool found, bool on_buffer, bool fullsize)
{
	atomic64_t *v;

	int i = 0;
	if (rw)
		i |= (1 << STAT_WRITE);
	if (found)
		i |= (1 << STAT_HIT);
	if (on_buffer)
		i |= (1 << STAT_ON_BUFFER);
	if (fullsize)
		i |= (1 << STAT_FULLSIZE);

	v = &wb->stat[i];
	atomic64_inc(v);
}

static void clear_stat(struct wb_device *wb)
{
	size_t i;
	for (i = 0; i < STATLEN; i++) {
		atomic64_t *v = &wb->stat[i];
		atomic64_set(v, 0);
	}
	atomic64_set(&wb->count_non_full_flushed, 0);
}

/*----------------------------------------------------------------------------*/

void inc_nr_dirty_caches(struct wb_device *wb)
{
	BUG_ON(!wb);
	atomic64_inc(&wb->nr_dirty_caches);
}

void dec_nr_dirty_caches(struct wb_device *wb)
{
	BUG_ON(!wb);
	if (atomic64_dec_and_test(&wb->nr_dirty_caches))
		wake_up_interruptible(&wb->wait_drop_caches);
}

static bool taint_mb(struct wb_device *wb, struct metablock *mb, u8 data_bits)
{
	unsigned long flags;
	bool flip = false;

	BUG_ON(data_bits == 0);
	spin_lock_irqsave(&wb->mb_lock, flags);
	if (!mb->dirtiness.is_dirty) {
		mb->dirtiness.is_dirty = true;
		flip = true;
	}
	mb->dirtiness.data_bits |= data_bits;
	spin_unlock_irqrestore(&wb->mb_lock, flags);

	return flip;
}

bool mark_clean_mb(struct wb_device *wb, struct metablock *mb)
{
	unsigned long flags;
	bool flip = false;

	spin_lock_irqsave(&wb->mb_lock, flags);
	if (mb->dirtiness.is_dirty) {
		mb->dirtiness.is_dirty = false;
		flip = true;
	}
	spin_unlock_irqrestore(&wb->mb_lock, flags);

	return flip;
}

/*
 * Read the dirtiness of a metablock at the moment.
 */
struct dirtiness read_mb_dirtiness(struct wb_device *wb, struct segment_header *seg,
				   struct metablock *mb)
{
	unsigned long flags;
	struct dirtiness retval;

	spin_lock_irqsave(&wb->mb_lock, flags);
	retval = mb->dirtiness;
	spin_unlock_irqrestore(&wb->mb_lock, flags);

	return retval;
}

/*----------------------------------------------------------------------------*/

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,14,0)
#define bv_vec struct bio_vec
#define bv_page(vec) vec.bv_page
#define bv_offset(vec) vec.bv_offset
#define bv_len(vec) vec.bv_len
#define bv_it struct bvec_iter
#else
#define bv_vec struct bio_vec *
#define bv_page(vec) vec->bv_page
#define bv_offset(vec) vec->bv_offset
#define bv_len(vec) vec->bv_len
#define bv_it int
#endif

/*
 * Incoming bio may have multiple bio vecs as a result bvec merging.
 * We shouldn't use bio_data directly to access to whole payload but
 * should iterate over the vector.
 */
static void copy_bio_payload(void *buf, struct bio *bio)
{
	size_t sum = 0;
	bv_vec vec;
	bv_it it;
	bio_for_each_segment(vec, bio, it) {
		void *dst = kmap_atomic(bv_page(vec));
		size_t l = bv_len(vec);
		memcpy(buf, dst + bv_offset(vec), l);
		kunmap_atomic(dst);
		buf += l;
		sum += l;
	}
	BUG_ON(sum != bi_size(bio));
}

/*
 * Copy 512B buffer data to bio payload's i-th 512B area.
 */
static void __copy_to_bio_payload(struct bio *bio, void *buf, u8 i)
{
	size_t head = 0;
	size_t tail = head;

	bv_vec vec;
	bv_it it;
	bio_for_each_segment(vec, bio, it) {
		size_t l = bv_len(vec);
		tail += l;
		if ((i << 9) < tail) {
			void *dst = kmap_atomic(bv_page(vec));
			size_t offset = (i << 9) - head;
			BUG_ON((l - offset) < (1 << 9));
			memcpy(dst + bv_offset(vec) + offset, buf, 1 << 9);
			kunmap_atomic(dst);
			return;
		}
		head += l;
	}
	BUG();
}

/*
 * Copy 4KB buffer to bio payload with care to bio offset and copy bits.
 */
static void copy_to_bio_payload(struct bio *bio, void *buf, u8 copy_bits)
{
	u8 offset = io_offset(bio);
	u8 i;
	for (i = 0; i < bio_sectors(bio); i++) {
		u8 i_offset = i + offset;
		if (copy_bits & (1 << i_offset))
			__copy_to_bio_payload(bio, buf + (i_offset << 9), i);
	}
}

static u8 to_mask(u8 offset, u8 count)
{
	u8 i;
	u8 result = 0;
	if (count == 8) {
		result = 255;
	} else {
		for (i = 0; i < count; i++)
			result |= (1 << (i + offset));
	}
	return result;
}

static int fill_payload_by_backing(struct wb_device *wb, struct bio *bio)
{
	struct dm_io_request io_req;
	struct dm_io_region region;

	sector_t start = bi_sector(bio);
	u8 offset = do_io_offset(start);
	u8 len = bio_sectors(bio);
	u8 copy_bits = to_mask(offset, len);

	int err = 0;
	void *buf = mempool_alloc(wb->buf_8_pool, GFP_NOIO);
	if (!buf)
		return -ENOMEM;

	io_req = (struct dm_io_request) {
		.client = wb->io_client,
		.bi_rw = READ,
		.notify.fn = NULL,
		.mem.type = DM_IO_KMEM,
		.mem.ptr.addr = buf + (offset << 9),
	};
	region = (struct dm_io_region) {
		.bdev = wb->backing_dev->bdev,
		.sector = start,
		.count = len,
	};
	err = wb_io(&io_req, 1, &region, NULL, true);
	if (err)
		goto bad;

	copy_to_bio_payload(bio, buf, copy_bits);
bad:
	mempool_free(buf, wb->buf_8_pool);
	return err;
}

/*
 * Get the reference to the 4KB-aligned data in RAM buffer.
 * Since it only takes the reference caller need not to free the pointer.
 */
static void *ref_buffered_mb(struct wb_device *wb, struct metablock *mb)
{
	sector_t offset = ((mb_idx_inseg(wb, mb->idx) + 1) << 3);
	return wb->current_rambuf->data + (offset << 9);
}

/*
 * Read cache block of the mb.
 * Caller should free the returned pointer after used by mempool_alloc().
 */
static void *read_mb(struct wb_device *wb, struct segment_header *seg,
		     struct metablock *mb, u8 data_bits)
{
	u8 i;
	void *result = mempool_alloc(wb->buf_8_pool, GFP_NOIO);
	if (!result)
		return NULL;

	for (i = 0; i < 8; i++) {
		int err = 0;
		struct dm_io_request io_req;
		struct dm_io_region region;

		if (!(data_bits & (1 << i)))
			continue;

		io_req = (struct dm_io_request) {
			.client = wb->io_client,
			.bi_rw = READ,
			.notify.fn = NULL,
			.mem.type = DM_IO_KMEM,
			.mem.ptr.addr = result + (i << 9),
		};

		region = (struct dm_io_region) {
			.bdev = wb->cache_dev->bdev,
			.sector = calc_mb_start_sector(wb, seg, mb->idx) + i,
			.count = 1,
		};

		err = wb_io(&io_req, 1, &region, NULL, true);
		if (err) {
			mempool_free(result, wb->buf_8_pool);
			return NULL;
		}
	}
	return result;
}

static void memcpy_masked(void *to, u8 protect_bits, void *from, u8 copy_bits)
{
	u8 i;
	for (i = 0; i < 8; i++) {
		bool will_copy = copy_bits & (1 << i);
		bool protected = protect_bits & (1 << i);
		if (will_copy && (!protected)) {
			size_t offset = (i << 9);
			memcpy(to + offset, from + offset, 1 << 9);
		}
	}
}

static void write_on_rambuffer(struct wb_device *wb, struct metablock *write_pos, struct write_io *wio)
{
	size_t mb_offset = (mb_idx_inseg(wb, write_pos->idx) + 1) << 12;
	void *mb_data = wb->current_rambuf->data + mb_offset;
	if (wio->data_bits == 255)
		memcpy(mb_data, wio->data, 1 << 12);
	else
		memcpy_masked(mb_data, 0, wio->data, wio->data_bits);
}

/*
 * Advance the cursor and return the old cursor.
 * After returned, nr_inflight_ios is incremented to wait for this write to complete.
 */
static u32 advance_cursor(struct wb_device *wb)
{
	u32 old;
	if (wb->cursor == wb->nr_caches)
		wb->cursor = 0;
	old = wb->cursor;
	wb->cursor++;
	wb->current_seg->length++;
	BUG_ON(wb->current_seg->length > wb->nr_caches_inseg);
	atomic_inc(&wb->current_seg->nr_inflight_ios);
	return old;
}

static bool needs_queue_seg(struct wb_device *wb)
{
	bool rambuf_no_space = !mb_idx_inseg(wb, wb->cursor);
	return rambuf_no_space;
}

/*
 * queue_current_buffer if the RAM buffer can't make space any more.
 */
static void might_queue_current_buffer(struct wb_device *wb)
{
	if (needs_queue_seg(wb)) {
		update_nr_empty_segs(wb);
		queue_current_buffer(wb);
	}
}

/*
 * Process bio with REQ_FLUSH
 */
static int process_flush_bio(struct wb_device *wb, struct bio *bio)
{
	/* In device-mapper bio with REQ_FLUSH is guaranteed to have no data. */
	BUG_ON(bi_size(bio));
	queue_barrier_io(wb, bio);
	return DM_MAPIO_SUBMITTED;
}

struct lookup_result {
	struct ht_head *head; /* Lookup head used */
	struct lookup_key key; /* Lookup key used */

	struct segment_header *found_seg;
	struct metablock *found_mb;

	bool found; /* Cache hit? */
	bool on_buffer; /* Is the metablock found on the RAM buffer? */
};

/*
 * Lookup a bio relevant cache data.
 * In case of cache hit, nr_inflight_ios is incremented.
 */
static void cache_lookup(struct wb_device *wb, struct bio *bio, struct lookup_result *res)
{
	res->key = (struct lookup_key) {
		.sector = calc_cache_alignment(bi_sector(bio)),
	};
	res->head = ht_get_head(wb, &res->key);

	res->found_mb = ht_lookup(wb, res->head, &res->key);
	if (res->found_mb) {
		res->found_seg = mb_to_seg(wb, res->found_mb);
		atomic_inc(&res->found_seg->nr_inflight_ios);
	}

	res->found = (res->found_mb != NULL);

	res->on_buffer = false;
	if (res->found)
		res->on_buffer = is_on_buffer(wb, res->found_mb->idx);

	inc_stat(wb, io_write(bio), res->found, res->on_buffer, io_fullsize(bio));
}

int prepare_overwrite(struct wb_device *wb, struct segment_header *seg, struct metablock *old_mb, struct write_io* wio, u8 overwrite_bits)
{
	struct dirtiness dirtiness = read_mb_dirtiness(wb, seg, old_mb);

	bool needs_merge_prev_cache = !(overwrite_bits == 255) || !(dirtiness.data_bits == 255);

	if (!dirtiness.is_dirty)
		needs_merge_prev_cache = false;

	if (overwrite_bits == 255)
		needs_merge_prev_cache = false;

	if (unlikely(needs_merge_prev_cache)) {
		void *buf;

		wait_for_flushing(wb, seg->id);
		BUG_ON(!dirtiness.is_dirty);

		buf = read_mb(wb, seg, old_mb, dirtiness.data_bits);
		if (!buf)
			return -EIO;

		/* newer data should be prioritized */
		memcpy_masked(wio->data, wio->data_bits, buf, dirtiness.data_bits);
		wio->data_bits |= dirtiness.data_bits;
		mempool_free(buf, wb->buf_8_pool);
	}

	if (mark_clean_mb(wb, old_mb))
		dec_nr_dirty_caches(wb);

	ht_del(wb, old_mb);

	return 0;
}

/*
 * Get a new place to write.
 */
static struct metablock *prepare_new_write_pos(struct wb_device *wb)
{
	struct metablock *ret = wb->current_seg->mb_array + mb_idx_inseg(wb, advance_cursor(wb));
	BUG_ON(ret->dirtiness.is_dirty);
	ret->dirtiness.data_bits = 0;
	BUG_ON(ret->dirtiness.data_bits);
	return ret;
}

static void dec_inflight_ios(struct wb_device *wb, struct segment_header *seg)
{
	if (atomic_dec_and_test(&seg->nr_inflight_ios))
		wake_up_active_wq(&wb->inflight_ios_wq);
}

static void initialize_write_io(struct write_io *wio, struct bio *bio)
{
	u8 offset = io_offset(bio);
	sector_t count = bio_sectors(bio);
	copy_bio_payload(wio->data + (offset << 9), bio);
	wio->data_bits = to_mask(offset, count);
}

static void might_cancel_read_cache_cell(struct wb_device *, struct bio *);
static int do_process_write(struct wb_device *wb, struct bio *bio)
{
	int retval = 0;

	struct metablock *write_pos = NULL;
	struct lookup_result res;

	struct write_io wio;
	wio.data = mempool_alloc(wb->buf_8_pool, GFP_NOIO);
	if (!wio.data)
		return -ENOMEM;
	initialize_write_io(&wio, bio);

	mutex_lock(&wb->io_lock);

	cache_lookup(wb, bio, &res);

	if (res.found) {
		if (unlikely(res.on_buffer)) {
			write_pos = res.found_mb;
			goto do_write;
		} else {
			retval = prepare_overwrite(wb, res.found_seg, res.found_mb, &wio, wio.data_bits);
			dec_inflight_ios(wb, res.found_seg);
			if (retval)
				goto out;
		}
	} else
		might_cancel_read_cache_cell(wb, bio);

	might_queue_current_buffer(wb);

	write_pos = prepare_new_write_pos(wb);

do_write:
	BUG_ON(write_pos == NULL);
	write_on_rambuffer(wb, write_pos, &wio);

	if (taint_mb(wb, write_pos, wio.data_bits))
		inc_nr_dirty_caches(wb);

	ht_register(wb, res.head, write_pos, &res.key);

out:
	mutex_unlock(&wb->io_lock);
	mempool_free(wio.data, wb->buf_8_pool);
	return retval;
}

static int complete_process_write(struct wb_device *wb, struct bio *bio)
{
	dec_inflight_ios(wb, wb->current_seg);

	/*
	 * bio with REQ_FUA has data.
	 * For such bio, we first treat it like a normal bio and then as a REQ_FLUSH bio.
	 */
	if (bio->bi_rw & REQ_FUA) {
		queue_barrier_io(wb, bio);
		return DM_MAPIO_SUBMITTED;
	}

	bio_endio_compat(bio, 0);
	return DM_MAPIO_SUBMITTED;
}

/*
 * (Locking) Dirtiness of a metablock
 * ----------------------------------
 * A cache data is placed either on RAM buffer or SSD if it was flushed.
 * To make locking easy, we simplify the rule for the dirtiness of a cache data.
 * 1) If the data is on the RAM buffer, the dirtiness only "increases".
 * 2) If the data is, on the other hand, on the SSD after flushed the dirtiness
 *    only "decreases".
 *
 * These simple rules can remove the possibility of dirtiness fluctuate on the
 * RAM buffer.
 */

/*
 * (Locking) Refcount (in_flight_*)
 * --------------------------------
 *
 * The basic common idea is
 * 1) Increment the refcount inside lock
 * 2) Wait for decrement outside the lock
 *
 * process_write:
 *   do_process_write:
 *     mutex_lock (to serialize write)
 *       inc in_flight_ios # refcount on the dst segment
 *     mutex_unlock
 *
 *   complete_process_write:
 *     dec in_flight_ios
 *     bio_endio(bio)
 */
static int process_write_wb(struct wb_device *wb, struct bio *bio)
{
	int err = do_process_write(wb, bio);
	if (err)
		return err;
	return complete_process_write(wb, bio);
}

static int process_write_wa(struct wb_device *wb, struct bio *bio)
{
	struct lookup_result res;

	mutex_lock(&wb->io_lock);
	cache_lookup(wb, bio, &res);
	if (res.found) {
		dec_inflight_ios(wb, res.found_seg);
		ht_del(wb, res.found_mb);
	}

	might_cancel_read_cache_cell(wb, bio);
	mutex_unlock(&wb->io_lock);

	bio_remap(bio, wb->backing_dev, bi_sector(bio));
	return DM_MAPIO_REMAPPED;
}

static int process_write(struct wb_device *wb, struct bio *bio)
{
	return wb->write_around_mode ? process_write_wa(wb, bio) : process_write_wb(wb, bio);
}

enum PBD_FLAG {
	PBD_NONE = 0,
	PBD_WILL_CACHE = 1,
	PBD_READ_SEG = 2,
};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,6,0)
#define PER_BIO_DATA_SIZE per_io_data_size
#else
#define PER_BIO_DATA_SIZE per_bio_data_size
#endif
struct per_bio_data {
	enum PBD_FLAG type;
	union {
		u32 cell_idx;
		struct segment_header *seg;
	};
};
#define per_bio_data(wb, bio) ((struct per_bio_data *)dm_per_bio_data((bio), (wb)->ti->PER_BIO_DATA_SIZE))

static void reserve_read_cache_cell(struct wb_device *, struct bio *);
static int process_read(struct wb_device *wb, struct bio *bio)
{
	struct lookup_result res;
	struct dirtiness dirtiness;
	struct per_bio_data *pbd;

	mutex_lock(&wb->io_lock);
	cache_lookup(wb, bio, &res);
	if (!res.found)
		reserve_read_cache_cell(wb, bio);
	mutex_unlock(&wb->io_lock);

	if (!res.found) {
		bio_remap(bio, wb->backing_dev, bi_sector(bio));
		return DM_MAPIO_REMAPPED;
	}

	dirtiness = read_mb_dirtiness(wb, res.found_seg, res.found_mb);
	if (unlikely(res.on_buffer)) {
		int err = fill_payload_by_backing(wb, bio);
		if (err)
			goto read_buffered_mb_exit;

		if (dirtiness.is_dirty)
			copy_to_bio_payload(bio, ref_buffered_mb(wb, res.found_mb), dirtiness.data_bits);

read_buffered_mb_exit:
		dec_inflight_ios(wb, res.found_seg);

		if (unlikely(err))
			bio_io_error(bio);
		else
			bio_endio_compat(bio, 0);

		return DM_MAPIO_SUBMITTED;
	}

	/*
	 * We need to wait for the segment to be flushed to the cache device.
	 * Without this, we might read the wrong data from the cache device.
	 */
	wait_for_flushing(wb, res.found_seg->id);

	if (unlikely(dirtiness.data_bits != 255)) {
		int err = fill_payload_by_backing(wb, bio);
		if (err)
			goto read_mb_exit;

		if (dirtiness.is_dirty) {
			void *buf = read_mb(wb, res.found_seg, res.found_mb, dirtiness.data_bits);
			if (!buf) {
				err = -EIO;
				goto read_mb_exit;
			}
			copy_to_bio_payload(bio, buf, dirtiness.data_bits);
			mempool_free(buf, wb->buf_8_pool);
		}

read_mb_exit:
		dec_inflight_ios(wb, res.found_seg);

		if (unlikely(err))
			bio_io_error(bio);
		else
			bio_endio_compat(bio, 0);

		return DM_MAPIO_SUBMITTED;
	}

	pbd = per_bio_data(wb, bio);
	pbd->type = PBD_READ_SEG;
	pbd->seg = res.found_seg;

	bio_remap(bio, wb->cache_dev,
		  calc_mb_start_sector(wb, res.found_seg, res.found_mb->idx) +
		  io_offset(bio));

	return DM_MAPIO_REMAPPED;
}

static int process_bio(struct wb_device *wb, struct bio *bio)
{
	return io_write(bio) ? process_write(wb, bio) : process_read(wb, bio);
}

static int writeboost_map(struct dm_target *ti, struct bio *bio)
{
	struct wb_device *wb = ti->private;

	struct per_bio_data *pbd = per_bio_data(wb, bio);
	pbd->type = PBD_NONE;

	if (bio->bi_rw & REQ_FLUSH)
		return process_flush_bio(wb, bio);

	return process_bio(wb, bio);
}

static void read_cache_cell_copy_data(struct wb_device *, struct bio*, int error);
static int writeboost_end_io(struct dm_target *ti, struct bio *bio, int error)
{
	struct wb_device *wb = ti->private;
	struct per_bio_data *pbd = per_bio_data(wb, bio);

	switch (pbd->type) {
	case PBD_NONE:
		return 0;
	case PBD_WILL_CACHE:
		read_cache_cell_copy_data(wb, bio, error);
		return 0;
	case PBD_READ_SEG:
		dec_inflight_ios(wb, pbd->seg);
		return 0;
	default:
		BUG();
	}
}

/*----------------------------------------------------------------------------*/

#define read_cache_cell_from_node(node) rb_entry((node), struct read_cache_cell, rb_node)

static void read_cache_add(struct read_cache_cells *cells, struct read_cache_cell *cell)
{
	struct rb_node **rbp, *parent;
	rbp = &cells->rb_root.rb_node;
	parent = NULL;
	while (*rbp) {
		struct read_cache_cell *parent_cell;
		parent = *rbp;
		parent_cell = read_cache_cell_from_node(parent);
		if (cell->sector < parent_cell->sector)
			rbp = &(*rbp)->rb_left;
		else
			rbp = &(*rbp)->rb_right;
	}
	rb_link_node(&cell->rb_node, parent, rbp);
	rb_insert_color(&cell->rb_node, &cells->rb_root);
}

static struct read_cache_cell *lookup_read_cache_cell(struct wb_device *wb, sector_t sector)
{
	struct rb_node **rbp, *parent;
	rbp = &wb->read_cache_cells->rb_root.rb_node;
	parent = NULL;
	while (*rbp) {
		struct read_cache_cell *parent_cell;
		parent = *rbp;
		parent_cell = read_cache_cell_from_node(parent);
		if (parent_cell->sector == sector)
			return parent_cell;

		if (sector < parent_cell->sector)
			rbp = &(*rbp)->rb_left;
		else
			rbp = &(*rbp)->rb_right;
	}
	return NULL;
}

static void read_cache_cancel_cells(struct read_cache_cells *cells, u32 n)
{
	u32 i;
	u32 last = cells->cursor + cells->seqcount;
	if (last > cells->size)
		last = cells->size;
	for (i = cells->cursor; i < last; i++) {
		struct read_cache_cell *cell = cells->array + i;
		cell->cancelled = true;
	}
}

/*
 * Track the forefront read address and cancel cells in case of over threshold.
 * If the cell is cancelled foreground, we can save the memory copy in the background.
 */
static void read_cache_cancel_foreground(struct read_cache_cells *cells,
					 struct read_cache_cell *new_cell)
{
	if (new_cell->sector == (cells->last_sector + 8))
		cells->seqcount++;
	else {
		cells->seqcount = 1;
		cells->over_threshold = false;
	}

	if (cells->seqcount > cells->threshold) {
		if (cells->over_threshold)
			new_cell->cancelled = true;
		else {
			cells->over_threshold = true;
			read_cache_cancel_cells(cells, cells->seqcount);
		}
	}
	cells->last_sector = new_cell->sector;
}

static void reserve_read_cache_cell(struct wb_device *wb, struct bio *bio)
{
	struct per_bio_data *pbd;
	struct read_cache_cells *cells = wb->read_cache_cells;
	struct read_cache_cell *found, *new_cell;

	BUG_ON(!cells->threshold);

	if (!ACCESS_ONCE(wb->read_cache_threshold))
		return;

	if (!cells->cursor)
		return;

	/*
	 * We only cache 4KB read data for following reasons:
	 * 1) Caching partial data (< 4KB) is likely meaningless.
	 * 2) Caching partial data makes the read-caching mechanism very hard.
	 */
	if (!io_fullsize(bio))
		return;

	/*
	 * We don't need to reserve the same address twice
	 * because it's either unchanged or invalidated.
	 */
	found = lookup_read_cache_cell(wb, bi_sector(bio));
	if (found)
		return;

	cells->cursor--;
	new_cell = cells->array + cells->cursor;
	new_cell->sector = bi_sector(bio);
	read_cache_add(cells, new_cell);

	pbd = per_bio_data(wb, bio);
	pbd->type = PBD_WILL_CACHE;
	pbd->cell_idx = cells->cursor;

	/* Cancel the new_cell if needed */
	read_cache_cancel_foreground(cells, new_cell);
}

static void might_cancel_read_cache_cell(struct wb_device *wb, struct bio *bio)
{
	struct read_cache_cell *found;
	found = lookup_read_cache_cell(wb, calc_cache_alignment(bi_sector(bio)));
	if (found)
		found->cancelled = true;
}

static void read_cache_cell_copy_data(struct wb_device *wb, struct bio *bio, int error)
{
	struct per_bio_data *pbd = per_bio_data(wb, bio);
	struct read_cache_cells *cells = wb->read_cache_cells;
	struct read_cache_cell *cell = cells->array + pbd->cell_idx;

	/* Data can be broken. So don't stage. */
	if (error)
		cell->cancelled = true;

	/*
	 * We can omit copying if the cell is cancelled but
	 * copying for a non-cancelled cell isn't problematic.
	 */
	if (!cell->cancelled)
		copy_bio_payload(cell->data, bio);

	if (atomic_dec_and_test(&cells->ack_count))
		queue_work(cells->wq, &wb->read_cache_work);
}

/*
 * Get a read cache cell through simplified write path if the cell data isn't stale.
 */
static void inject_read_cache(struct wb_device *wb, struct read_cache_cell *cell)
{
	struct metablock *mb;
	u32 _mb_idx_inseg;
	struct segment_header *seg;

	struct lookup_key key = {
		.sector = cell->sector,
	};
	struct ht_head *head = ht_get_head(wb, &key);

	mutex_lock(&wb->io_lock);
	/*
	 * if might_cancel_read_cache_cell() on the foreground
	 * cancelled this cell, the data is now stale.
	 */
	if (cell->cancelled) {
		mutex_unlock(&wb->io_lock);
		return;
	}

	might_queue_current_buffer(wb);

	seg = wb->current_seg;
	_mb_idx_inseg = mb_idx_inseg(wb, advance_cursor(wb));

	/*
	 * We should copy the cell data into the rambuf with lock held
	 * otherwise subsequent write data may be written first and then overwritten by
	 * the old data in the cell.
	 */
	memcpy(wb->current_rambuf->data + ((_mb_idx_inseg + 1) << 12), cell->data, 1 << 12);

	mb = seg->mb_array + _mb_idx_inseg;
	BUG_ON(mb->dirtiness.is_dirty);
	mb->dirtiness.data_bits = 255;

	ht_register(wb, head, mb, &key);

	mutex_unlock(&wb->io_lock);

	dec_inflight_ios(wb, seg);
}

static void free_read_cache_cell_data(struct read_cache_cells *cells)
{
	u32 i;
	for (i = 0; i < cells->size; i++) {
		struct read_cache_cell *cell = cells->array + i;
		vfree(cell->data);
	}
}

static struct read_cache_cells *alloc_read_cache_cells(struct wb_device *wb, u32 n)
{
	struct read_cache_cells *cells;
	u32 i;
	cells = kmalloc(sizeof(struct read_cache_cells), GFP_KERNEL);
	if (!cells)
		return NULL;

	cells->size = n;
	cells->threshold = UINT_MAX; /* Default: every read will be cached */
	cells->last_sector = ~0;
	cells->seqcount = 0;
	cells->over_threshold = false;
	cells->array = kmalloc(sizeof(struct read_cache_cell) * n, GFP_KERNEL);
	if (!cells->array)
		goto bad_cells_array;

	for (i = 0; i < cells->size; i++) {
		struct read_cache_cell *cell = cells->array + i;
		cell->data = vmalloc(1 << 12);
		if (!cell->data) {
			u32 j;
			for (j = 0; j < i; j++) {
				cell = cells->array + j;
				vfree(cell->data);
			}
			goto bad_cell_data;
		}
	}

	cells->wq = create_singlethread_workqueue("dmwb_read_cache");
	if (!cells->wq)
		goto bad_wq;

	return cells;

bad_wq:
	free_read_cache_cell_data(cells);
bad_cell_data:
	kfree(cells->array);
bad_cells_array:
	kfree(cells);
	return NULL;
}

static void free_read_cache_cells(struct wb_device *wb)
{
	struct read_cache_cells *cells = wb->read_cache_cells;
	destroy_workqueue(cells->wq); /* This drains wq. So, must precede the others */
	free_read_cache_cell_data(cells);
	kfree(cells->array);
	kfree(cells);
}

static void reinit_read_cache_cells(struct wb_device *wb)
{
	struct read_cache_cells *cells = wb->read_cache_cells;
	u32 i, cur_threshold;
	for (i = 0; i < cells->size; i++) {
		struct read_cache_cell *cell = cells->array + i;
		cell->cancelled = false;
	}
	atomic_set(&cells->ack_count, cells->size);

	mutex_lock(&wb->io_lock);
	cells->rb_root = RB_ROOT;
	cells->cursor = cells->size;
	cur_threshold = ACCESS_ONCE(wb->read_cache_threshold);
	if (cur_threshold && (cur_threshold != cells->threshold)) {
		cells->threshold = cur_threshold;
		cells->over_threshold = false;
	}
	mutex_unlock(&wb->io_lock);
}

/*
 * Cancel cells [first, last)
 */
static void visit_and_cancel_cells(struct rb_node *first, struct rb_node *last)
{
	struct rb_node *rbp = first;
	while (rbp != last) {
		struct read_cache_cell *cell = read_cache_cell_from_node(rbp);
		cell->cancelled = true;
		rbp = rb_next(rbp);
	}
}

/*
 * Find out sequence from cells and cancel them if larger than threshold.
 */
static void read_cache_cancel_background(struct read_cache_cells *cells)
{
	struct rb_node *rbp = rb_first(&cells->rb_root);
	struct rb_node *seqhead = rbp;
	sector_t last_sector = ~0;
	u32 seqcount = 0;

	while (rbp) {
		struct read_cache_cell *cell = read_cache_cell_from_node(rbp);
		if (cell->sector == (last_sector + 8))
			seqcount++;
		else {
			if (seqcount > cells->threshold)
				visit_and_cancel_cells(seqhead, rbp);
			seqcount = 1;
			seqhead = rbp;
		}
		last_sector = cell->sector;
		rbp = rb_next(rbp);
	}
	if (seqcount > cells->threshold)
		visit_and_cancel_cells(seqhead, rbp);
}

static void read_cache_proc(struct work_struct *work)
{
	struct wb_device *wb = container_of(work, struct wb_device, read_cache_work);
	struct read_cache_cells *cells = wb->read_cache_cells;
	u32 i;

	read_cache_cancel_background(cells);

	for (i = 0; i < cells->size; i++) {
		struct read_cache_cell *cell = cells->array + i;
		inject_read_cache(wb, cell);
	}
	reinit_read_cache_cells(wb);
}

static int init_read_cache_cells(struct wb_device *wb)
{
	struct read_cache_cells *cells;
	INIT_WORK(&wb->read_cache_work, read_cache_proc);
	cells = alloc_read_cache_cells(wb, wb->nr_read_cache_cells);
	if (!cells)
		return -ENOMEM;
	wb->read_cache_cells = cells;
	reinit_read_cache_cells(wb);
	return 0;
}

/*----------------------------------------------------------------------------*/

static int consume_essential_argv(struct wb_device *wb, struct dm_arg_set *as)
{
	int r = 0;
	struct dm_target *ti = wb->ti;

	r = dm_get_device(ti, dm_shift_arg(as), dm_table_get_mode(ti->table),
			  &wb->backing_dev);
	if (r) {
		DMERR("Failed to get backing_dev");
		return r;
	}

	r = dm_get_device(ti, dm_shift_arg(as), dm_table_get_mode(ti->table),
			  &wb->cache_dev);
	if (r) {
		DMERR("Failed to get cache_dev");
		goto bad_get_cache;
	}

	return r;

bad_get_cache:
	dm_put_device(ti, wb->backing_dev);
	return r;
}

#define consume_kv(name, nr, is_static) { \
	if (!strcasecmp(key, #name)) { \
		if (!argc) \
			break; \
		if (test_bit(WB_CREATED, &wb->flags) && is_static) { \
			DMERR("%s is a static option", #name); \
			break; \
		} \
		r = dm_read_arg(_args + (nr), as, &tmp, &ti->error); \
		if (r) { \
			DMERR("%s", ti->error); \
			break; \
		} \
		wb->name = tmp; \
	 } }

static int do_consume_optional_argv(struct wb_device *wb, struct dm_arg_set *as, unsigned argc)
{
	int r = 0;
	struct dm_target *ti = wb->ti;

	static struct dm_arg _args[] = {
		{0, 100, "Invalid writeback_threshold"},
		{1, 32, "Invalid nr_max_batched_writeback"},
		{0, 3600, "Invalid update_sb_record_interval"},
		{0, 3600, "Invalid sync_data_interval"},
		{0, 127, "Invalid read_cache_threshold"},
		{0, 1, "Invalid write_around_mode"},
		{1, 2048, "Invalid nr_read_cache_cells"},
	};
	unsigned tmp;

	while (argc) {
		const char *key = dm_shift_arg(as);
		argc--;

		r = -EINVAL;

		consume_kv(writeback_threshold, 0, false);
		consume_kv(nr_max_batched_writeback, 1, false);
		consume_kv(update_sb_record_interval, 2, false);
		consume_kv(sync_data_interval, 3, false);
		consume_kv(read_cache_threshold, 4, false);
		consume_kv(write_around_mode, 5, true);
		consume_kv(nr_read_cache_cells, 6, true);

		if (!r) {
			argc--;
		} else {
			ti->error = "Invalid optional key";
			break;
		}
	}

	return r;
}

static int consume_optional_argv(struct wb_device *wb, struct dm_arg_set *as)
{
	int r = 0;
	struct dm_target *ti = wb->ti;

	static struct dm_arg _args[] = {
		{0, 14, "Invalid optional argc"},
	};
	unsigned argc = 0;

	if (as->argc) {
		r = dm_read_arg_group(_args, as, &argc, &ti->error);
		if (r) {
			DMERR("%s", ti->error);
			return r;
		}
	}

	return do_consume_optional_argv(wb, as, argc);
}

DECLARE_DM_KCOPYD_THROTTLE_WITH_MODULE_PARM(wb_copy_throttle,
		"A percentage of time allocated for one-shot writeback");

static int init_core_struct(struct dm_target *ti)
{
	int r = 0;
	struct wb_device *wb;

	r = dm_set_target_max_io_len(ti, 1 << 3);
	if (r) {
		DMERR("Failed to set max_io_len");
		return r;
	}

	ti->num_flush_bios = 1;
	ti->flush_supported = true;

	/*
	 * dm-writeboost does't support TRIM
	 *
	 * https://github.com/akiradeveloper/dm-writeboost/issues/110
	 * - discarding backing data only violates DRAT
	 * - strictly discarding both cache blocks and backing data is nearly impossible
	 *   considering cache hits may occur partially.
	 */
	ti->num_discard_bios = 0;
	ti->discards_supported = false;

	ti->PER_BIO_DATA_SIZE = sizeof(struct per_bio_data);

	wb = kzalloc(sizeof(*wb), GFP_KERNEL);
	if (!wb) {
		DMERR("Failed to allocate wb");
		return -ENOMEM;
	}
	ti->private = wb;
	wb->ti = ti;

	// init_waitqueue_head(&wb->writeback_mb_wait_queue);
	wb->copier = dm_kcopyd_client_create(&dm_kcopyd_throttle);
	if (IS_ERR(wb->copier)) {
		r = PTR_ERR(wb->copier);
		goto bad_kcopyd_client;
	}

	wb->buf_1_cachep = kmem_cache_create("dmwb_buf_1",
			1 << 9, 1 << 9, SLAB_RED_ZONE, NULL);
	if (!wb->buf_1_cachep) {
		r = -ENOMEM;
		goto bad_buf_1_cachep;
	}
	wb->buf_1_pool = mempool_create_slab_pool(16, wb->buf_1_cachep);
	if (!wb->buf_1_pool) {
		r = -ENOMEM;
		goto bad_buf_1_pool;
	}

	wb->buf_8_cachep = kmem_cache_create("dmwb_buf_8",
			1 << 12, 1 << 12, SLAB_RED_ZONE, NULL);
	if (!wb->buf_8_cachep) {
		r = -ENOMEM;
		goto bad_buf_8_cachep;
	}
	wb->buf_8_pool = mempool_create_slab_pool(16, wb->buf_8_cachep);
	if (!wb->buf_8_pool) {
		r = -ENOMEM;
		goto bad_buf_8_pool;
	}

	wb->io_wq = create_singlethread_workqueue("dmwb_io");
	if (!wb->io_wq) {
		DMERR("Failed to allocate io_wq");
		r = -ENOMEM;
		goto bad_io_wq;
	}

	wb->io_client = dm_io_client_create();
	if (IS_ERR(wb->io_client)) {
		DMERR("Failed to allocate io_client");
		r = PTR_ERR(wb->io_client);
		goto bad_io_client;
	}

	mutex_init(&wb->io_lock);
	init_waitqueue_head(&wb->inflight_ios_wq);
	spin_lock_init(&wb->mb_lock);
	atomic64_set(&wb->nr_dirty_caches, 0);
	clear_bit(WB_CREATED, &wb->flags);

	return r;

bad_io_client:
	destroy_workqueue(wb->io_wq);
bad_io_wq:
	mempool_destroy(wb->buf_8_pool);
bad_buf_8_pool:
	kmem_cache_destroy(wb->buf_8_cachep);
bad_buf_8_cachep:
	mempool_destroy(wb->buf_1_pool);
bad_buf_1_pool:
	kmem_cache_destroy(wb->buf_1_cachep);
bad_buf_1_cachep:
	dm_kcopyd_client_destroy(wb->copier);
bad_kcopyd_client:
	kfree(wb);
	return r;
}

static void free_core_struct(struct wb_device *wb)
{
	dm_io_client_destroy(wb->io_client);
	destroy_workqueue(wb->io_wq);
	mempool_destroy(wb->buf_8_pool);
	kmem_cache_destroy(wb->buf_8_cachep);
	mempool_destroy(wb->buf_1_pool);
	kmem_cache_destroy(wb->buf_1_cachep);
	dm_kcopyd_client_destroy(wb->copier);
	kfree(wb);
}

static int copy_ctr_args(struct wb_device *wb, int argc, const char **argv)
{
	unsigned i;
	const char **copy;

	copy = kcalloc(argc, sizeof(*copy), GFP_KERNEL);
	if (!copy)
		return -ENOMEM;
	for (i = 0; i < argc; i++) {
		copy[i] = kstrdup(argv[i], GFP_KERNEL);
		if (!copy[i]) {
			while (i--)
				kfree(copy[i]);
			kfree(copy);
			return -ENOMEM;
		}
	}

	wb->nr_ctr_args = argc;
	wb->ctr_args = copy;

	return 0;
}

static void free_ctr_args(struct wb_device *wb)
{
	int i;
	for (i = 0; i < wb->nr_ctr_args; i++)
		kfree(wb->ctr_args[i]);
	kfree(wb->ctr_args);
}

#define save_arg(name) wb->name##_saved = wb->name
#define restore_arg(name) if (wb->name##_saved) { wb->name = wb->name##_saved; }

/*
 * Create a writeboost device
 *
 * <essential args>
 * <#optional args> <optional args>
 * optionals are unordered lists of k-v pair.
 *
 * See doc for detail.
  */
static int writeboost_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	int r = 0;
	struct wb_device *wb;

	struct dm_arg_set as;
	as.argc = argc;
	as.argv = argv;

	r = init_core_struct(ti);
	if (r) {
		ti->error = "init_core_struct failed";
		return r;
	}
	wb = ti->private;

	r = copy_ctr_args(wb, argc - 2, (const char **)argv + 2);
	if (r) {
		ti->error = "copy_ctr_args failed";
		goto bad_ctr_args;
	}

	r = consume_essential_argv(wb, &as);
	if (r) {
		ti->error = "consume_essential_argv failed";
		goto bad_essential_argv;
	}

	r = consume_optional_argv(wb, &as);
	if (r) {
		ti->error = "consume_optional_argv failed";
		goto bad_optional_argv;
	}

	save_arg(writeback_threshold);
	save_arg(nr_max_batched_writeback);
	save_arg(update_sb_record_interval);
	save_arg(sync_data_interval);
	save_arg(read_cache_threshold);
	save_arg(nr_read_cache_cells);

	r = resume_cache(wb);
	if (r) {
		ti->error = "resume_cache failed";
		goto bad_resume_cache;
	}

	wb->nr_read_cache_cells = 2048; /* 8MB */
	restore_arg(nr_read_cache_cells);
	r = init_read_cache_cells(wb);
	if (r) {
		ti->error = "init_read_cache_cells failed";
		goto bad_read_cache_cells;
	}

	clear_stat(wb);

	set_bit(WB_CREATED, &wb->flags);

	restore_arg(writeback_threshold);
	restore_arg(nr_max_batched_writeback);
	restore_arg(update_sb_record_interval);
	restore_arg(sync_data_interval);
	restore_arg(read_cache_threshold);

	return r;

bad_read_cache_cells:
	free_cache(wb);
bad_resume_cache:
	dm_put_device(ti, wb->cache_dev);
	dm_put_device(ti, wb->backing_dev);
bad_optional_argv:
bad_essential_argv:
	free_ctr_args(wb);
bad_ctr_args:
	free_core_struct(wb);
	ti->private = NULL;

	return r;
}

static void writeboost_dtr(struct dm_target *ti)
{
	struct wb_device *wb = ti->private;

	free_read_cache_cells(wb);

	free_cache(wb);

	dm_put_device(ti, wb->cache_dev);
	dm_put_device(ti, wb->backing_dev);

	free_ctr_args(wb);

	free_core_struct(wb);
	ti->private = NULL;
}

/*----------------------------------------------------------------------------*/

/*
 * .postsuspend is called before .dtr.
 * We flush out all the transient data and make them persistent.
 */
static void writeboost_postsuspend(struct dm_target *ti)
{
	struct wb_device *wb = ti->private;
	flush_current_buffer(wb);
	blkdev_issue_flush(wb->cache_dev->bdev, GFP_NOIO, NULL);
}

static int writeboost_message(struct dm_target *ti, unsigned argc, char **argv)
{
	struct wb_device *wb = ti->private;

	struct dm_arg_set as;
	as.argc = argc;
	as.argv = argv;

	if (!strcasecmp(argv[0], "clear_stat")) {
		clear_stat(wb);
		return 0;
	}

	if (!strcasecmp(argv[0], "drop_caches")) {
		int r = 0;
		wb->force_drop = true;
		r = wait_event_interruptible(wb->wait_drop_caches,
			     !atomic64_read(&wb->nr_dirty_caches));
		wb->force_drop = false;
		return r;
	}

	return do_consume_optional_argv(wb, &as, 2);
}

/*
 * Since Writeboost is just a cache target and the cache block size is fixed
 * to 4KB. There is no reason to count the cache device in device iteration.
 */
static int writeboost_iterate_devices(struct dm_target *ti,
				      iterate_devices_callout_fn fn, void *data)
{
	struct wb_device *wb = ti->private;
	struct dm_dev *backing = wb->backing_dev;
	sector_t start = 0;
	sector_t len = dm_devsize(backing);
	return fn(ti, backing, start, len, data);
}

static void writeboost_io_hints(struct dm_target *ti, struct queue_limits *limits)
{
	blk_limits_io_opt(limits, 4096);
}

static void writeboost_status(struct dm_target *ti, status_type_t type,
			      unsigned flags, char *result, unsigned maxlen)
{
	ssize_t sz = 0;
	char buf[BDEVNAME_SIZE];
	struct wb_device *wb = ti->private;
	size_t i;

	switch (type) {
	case STATUSTYPE_INFO:
		DMEMIT("%u %u %llu %llu %llu %llu %llu",
		       (unsigned int)
		       wb->cursor,
		       (unsigned int)
		       wb->nr_caches,
		       (long long unsigned int)
		       wb->nr_segments,
		       (long long unsigned int)
		       wb->current_seg->id,
		       (long long unsigned int)
		       atomic64_read(&wb->last_flushed_segment_id),
		       (long long unsigned int)
		       atomic64_read(&wb->last_writeback_segment_id),
		       (long long unsigned int)
		       atomic64_read(&wb->nr_dirty_caches));

		for (i = 0; i < STATLEN; i++) {
			atomic64_t *v = &wb->stat[i];
			DMEMIT(" %llu", (unsigned long long) atomic64_read(v));
		}
		DMEMIT(" %llu", (unsigned long long) atomic64_read(&wb->count_non_full_flushed));

		DMEMIT(" %d", 10);
		DMEMIT(" writeback_threshold %d",
		       wb->writeback_threshold);
		DMEMIT(" nr_cur_batched_writeback %u",
		       wb->nr_cur_batched_writeback);
		DMEMIT(" sync_data_interval %lu",
		       wb->sync_data_interval);
		DMEMIT(" update_sb_record_interval %lu",
		       wb->update_sb_record_interval);
		DMEMIT(" read_cache_threshold %u",
		       wb->read_cache_threshold);
		break;

	case STATUSTYPE_TABLE:
		format_dev_t(buf, wb->backing_dev->bdev->bd_dev);
		DMEMIT(" %s", buf);
		format_dev_t(buf, wb->cache_dev->bdev->bd_dev);
		DMEMIT(" %s", buf);

		for (i = 0; i < wb->nr_ctr_args; i++)
			DMEMIT(" %s", wb->ctr_args[i]);
		break;
	}
}

static struct target_type writeboost_target = {
	.name = "writeboost",
	.version = {2, 2, 5},
	.module = THIS_MODULE,
	.map = writeboost_map,
	.end_io = writeboost_end_io,
	.ctr = writeboost_ctr,
	.dtr = writeboost_dtr,
	.postsuspend = writeboost_postsuspend,
	.message = writeboost_message,
	.status = writeboost_status,
	.io_hints = writeboost_io_hints,
	.iterate_devices = writeboost_iterate_devices,
};

static int __init writeboost_module_init(void)
{
	int r = 0;

	r = dm_register_target(&writeboost_target);
	if (r < 0) {
		DMERR("Failed to register target");
		return r;
	}

	return r;
}

static void __exit writeboost_module_exit(void)
{
	dm_unregister_target(&writeboost_target);
}

module_init(writeboost_module_init);
module_exit(writeboost_module_exit);

MODULE_AUTHOR("Akira Hayakawa <ruby.wktk@gmail.com>");
MODULE_DESCRIPTION(DM_NAME " writeboost target");
MODULE_LICENSE("GPL");
