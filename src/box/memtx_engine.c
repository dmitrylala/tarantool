/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "memtx_engine.h"
#include "memtx_space.h"

#include <small/quota.h>
#include <small/small.h>
#include <small/mempool.h>

#include "fiber.h"
#include "errinj.h"
#include "coio_file.h"
#include "tuple.h"
#include "txn.h"
#include "memtx_tree.h"
#include "iproto_constants.h"
#include "xrow.h"
#include "xstream.h"
#include "bootstrap.h"
#include "replication.h"
#include "schema.h"
#include "gc.h"

/* sync snapshot every 16MB */
#define SNAP_SYNC_INTERVAL	(1 << 24)

static void
checkpoint_cancel(struct checkpoint *ckpt);

struct PACKED memtx_tuple {
	/*
	 * sic: the header of the tuple is used
	 * to store a free list pointer in smfree_delayed.
	 * Please don't change it without understanding
	 * how smfree_delayed and snapshotting COW works.
	 */
	/** Snapshot generation version. */
	uint32_t version;
	struct tuple base;
};

enum {
	OBJSIZE_MIN = 16,
	SLAB_SIZE = 16 * 1024 * 1024,
	MAX_TUPLE_SIZE = 1 * 1024 * 1024,
};

static int
memtx_end_build_primary_key(struct space *space, void *param)
{
	struct memtx_space *memtx_space = (struct memtx_space *)space;
	if (space->engine != param || space_index(space, 0) == NULL ||
	    memtx_space->replace == memtx_space_replace_all_keys)
		return 0;

	index_end_build(space->index[0]);
	memtx_space->replace = memtx_space_replace_primary_key;
	return 0;
}

/**
 * Secondary indexes are built in bulk after all data is
 * recovered. This function enables secondary keys on a space.
 * Data dictionary spaces are an exception, they are fully
 * built right from the start.
 */
static int
memtx_build_secondary_keys(struct space *space, void *param)
{
	struct memtx_space *memtx_space = (struct memtx_space *)space;
	if (space->engine != param || space_index(space, 0) == NULL ||
	    memtx_space->replace == memtx_space_replace_all_keys)
		return 0;

	if (space->index_id_max > 0) {
		struct index *pk = space->index[0];
		ssize_t n_tuples = index_size(pk);
		assert(n_tuples >= 0);

		if (n_tuples > 0) {
			say_info("Building secondary indexes in space '%s'...",
				 space_name(space));
		}

		for (uint32_t j = 1; j < space->index_count; j++) {
			if (index_build(space->index[j], pk) < 0)
				return -1;
		}

		if (n_tuples > 0) {
			say_info("Space '%s': done", space_name(space));
		}
	}
	memtx_space->replace = memtx_space_replace_all_keys;
	return 0;
}

static void
memtx_engine_shutdown(struct engine *engine)
{
	struct memtx_engine *memtx = (struct memtx_engine *)engine;
	if (memtx->checkpoint != NULL)
		checkpoint_cancel(memtx->checkpoint);
	mempool_destroy(&memtx->iterator_pool);
	if (mempool_is_initialized(&memtx->rtree_iterator_pool))
		mempool_destroy(&memtx->rtree_iterator_pool);
	mempool_destroy(&memtx->index_extent_pool);
	slab_cache_destroy(&memtx->index_slab_cache);
	small_alloc_destroy(&memtx->alloc);
	slab_cache_destroy(&memtx->slab_cache);
	tuple_arena_destroy(&memtx->arena);
	xdir_destroy(&memtx->snap_dir);
	free(memtx);
}

static int
memtx_engine_recover_snapshot_row(struct memtx_engine *memtx,
				  struct xrow_header *row);

int
memtx_engine_recover_snapshot(struct memtx_engine *memtx,
			      const struct vclock *vclock)
{
	/* Process existing snapshot */
	say_info("recovery start");
	int64_t signature = vclock_sum(vclock);
	const char *filename = xdir_format_filename(&memtx->snap_dir,
						    signature, NONE);

	say_info("recovering from `%s'", filename);
	struct xlog_cursor cursor;
	if (xlog_cursor_open(&cursor, filename) < 0)
		return -1;

	int rc;
	struct xrow_header row;
	uint64_t row_count = 0;
	while ((rc = xlog_cursor_next(&cursor, &row,
				      memtx->force_recovery)) == 0) {
		row.lsn = signature;
		rc = memtx_engine_recover_snapshot_row(memtx, &row);
		if (rc < 0) {
			if (!memtx->force_recovery)
				break;
			say_error("can't apply row: ");
			diag_log();
		}
		++row_count;
		if (row_count % 100000 == 0) {
			say_info("%.1fM rows processed",
				 row_count / 1000000.);
			fiber_yield_timeout(0);
		}
	}
	xlog_cursor_close(&cursor, false);
	if (rc < 0)
		return -1;

	/**
	 * We should never try to read snapshots with no EOF
	 * marker - such snapshots are very likely corrupted and
	 * should not be trusted.
	 */
	if (!xlog_cursor_is_eof(&cursor))
		panic("snapshot `%s' has no EOF marker", filename);

	return 0;
}

static int
memtx_engine_recover_snapshot_row(struct memtx_engine *memtx,
				  struct xrow_header *row)
{
	assert(row->bodycnt == 1); /* always 1 for read */
	if (row->type != IPROTO_INSERT) {
		diag_set(ClientError, ER_UNKNOWN_REQUEST_TYPE,
			 (uint32_t) row->type);
		return -1;
	}

	struct request request;
	if (xrow_decode_dml(row, &request, dml_request_key_map(row->type)) != 0)
		return -1;
	struct space *space = space_cache_find(request.space_id);
	if (space == NULL)
		return -1;
	/* memtx snapshot must contain only memtx spaces */
	if (space->engine != (struct engine *)memtx) {
		diag_set(ClientError, ER_CROSS_ENGINE_TRANSACTION);
		return -1;
	}
	struct txn *txn = txn_begin();
	if (txn == NULL)
		return -1;
	/* no access checks here - applier always works with admin privs */
	if (space_apply_initial_join_row(space, &request) != 0) {
		txn_rollback(txn);
		fiber_gc();
		return -1;
	}
	int rc = txn_commit(txn);
	/*
	 * Don't let gc pool grow too much. Yet to
	 * it before reading the next row, to make
	 * sure it's not freed along here.
	 */
	fiber_gc();
	return rc;
}

/** Called at start to tell memtx to recover to a given LSN. */
static int
memtx_engine_begin_initial_recovery(struct engine *engine,
				    const struct vclock *vclock)
{
	(void)vclock;
	struct memtx_engine *memtx = (struct memtx_engine *)engine;
	assert(memtx->state == MEMTX_INITIALIZED);
	/*
	 * By default, enable fast start: bulk read of tuples
	 * from the snapshot, in which they are stored in key
	 * order, and bulk build of the primary key.
	 *
	 * If force_recovery = true, it's a disaster
	 * recovery mode. Enable all keys on start, to detect and
	 * discard duplicates in the snapshot.
	 */
	memtx->state = (memtx->force_recovery ?
			MEMTX_OK : MEMTX_INITIAL_RECOVERY);
	return 0;
}

static int
memtx_engine_begin_final_recovery(struct engine *engine)
{
	struct memtx_engine *memtx = (struct memtx_engine *)engine;
	if (memtx->state == MEMTX_OK)
		return 0;

	assert(memtx->state == MEMTX_INITIAL_RECOVERY);
	/* End of the fast path: loaded the primary key. */
	space_foreach(memtx_end_build_primary_key, memtx);

	if (!memtx->force_recovery) {
		/*
		 * Fast start path: "play out" WAL
		 * records using the primary key only,
		 * then bulk-build all secondary keys.
		 */
		memtx->state = MEMTX_FINAL_RECOVERY;
	} else {
		/*
		 * If force_recovery = true, it's
		 * a disaster recovery mode. Build
		 * secondary keys before reading the WAL,
		 * to detect and discard duplicates in
		 * unique keys.
		 */
		memtx->state = MEMTX_OK;
		if (space_foreach(memtx_build_secondary_keys, memtx) != 0)
			return -1;
	}
	return 0;
}

static int
memtx_engine_end_recovery(struct engine *engine)
{
	struct memtx_engine *memtx = (struct memtx_engine *)engine;
	/*
	 * Recovery is started with enabled keys when:
	 * - either of force_recovery
	 *   is false
	 * - it's a replication join
	 */
	if (memtx->state != MEMTX_OK) {
		assert(memtx->state == MEMTX_FINAL_RECOVERY);
		memtx->state = MEMTX_OK;
		if (space_foreach(memtx_build_secondary_keys, memtx) != 0)
			return -1;
	}
	xdir_collect_inprogress(&memtx->snap_dir);
	return 0;
}

static struct space *
memtx_engine_create_space(struct engine *engine, struct space_def *def,
			  struct rlist *key_list)
{
	struct memtx_engine *memtx = (struct memtx_engine *)engine;
	return memtx_space_new(memtx, def, key_list);
}

static int
memtx_engine_begin(struct engine *engine, struct txn *txn)
{
	(void)engine;
	txn_can_yield(txn, false);
	return 0;
}

static void
memtx_engine_rollback_statement(struct engine *engine, struct txn *txn,
				struct txn_stmt *stmt)
{
	(void)engine;
	(void)txn;
	if (stmt->old_tuple == NULL && stmt->new_tuple == NULL)
		return;
	struct space *space = stmt->space;
	struct memtx_space *memtx_space = (struct memtx_space *)space;
	int index_count;

	/* Only roll back the changes if they were made. */
	if (stmt->engine_savepoint == NULL)
		return;

	if (memtx_space->replace == memtx_space_replace_all_keys)
		index_count = space->index_count;
	else if (memtx_space->replace == memtx_space_replace_primary_key)
		index_count = 1;
	else
		panic("transaction rolled back during snapshot recovery");

	for (int i = 0; i < index_count; i++) {
		struct tuple *unused;
		struct index *index = space->index[i];
		/* Rollback must not fail. */
		if (index_replace(index, stmt->new_tuple, stmt->old_tuple,
				  DUP_INSERT, &unused) != 0) {
			diag_log();
			unreachable();
			panic("failed to rollback change");
		}
	}

	memtx_space_update_bsize(space, stmt->new_tuple, stmt->old_tuple);
	if (stmt->old_tuple != NULL)
		tuple_ref(stmt->old_tuple);
	if (stmt->new_tuple != NULL)
		tuple_unref(stmt->new_tuple);
}

static int
memtx_engine_bootstrap(struct engine *engine)
{
	struct memtx_engine *memtx = (struct memtx_engine *)engine;

	assert(memtx->state == MEMTX_INITIALIZED);
	memtx->state = MEMTX_OK;

	/* Recover from bootstrap.snap */
	say_info("initializing an empty data directory");
	struct xlog_cursor cursor;
	if (xlog_cursor_openmem(&cursor, (const char *)bootstrap_bin,
				sizeof(bootstrap_bin), "bootstrap") < 0)
		return -1;

	int rc;
	struct xrow_header row;
	while ((rc = xlog_cursor_next(&cursor, &row, true)) == 0) {
		rc = memtx_engine_recover_snapshot_row(memtx, &row);
		if (rc < 0)
			break;
	}
	xlog_cursor_close(&cursor, false);
	return rc < 0 ? -1 : 0;
}

static int
checkpoint_write_row(struct xlog *l, struct xrow_header *row)
{
	static ev_tstamp last = 0;
	if (last == 0) {
		ev_now_update(loop());
		last = ev_now(loop());
	}

	row->tm = last;
	row->replica_id = 0;
	/**
	 * Rows in snapshot are numbered from 1 to %rows.
	 * This makes streaming such rows to a replica or
	 * to recovery look similar to streaming a normal
	 * WAL. @sa the place which skips old rows in
	 * recovery_apply_row().
	 */
	row->lsn = l->rows + l->tx_rows;
	row->sync = 0; /* don't write sync to wal */

	ssize_t written = xlog_write_row(l, row);
	fiber_gc();
	if (written < 0)
		return -1;

	if ((l->rows + l->tx_rows) % 100000 == 0)
		say_crit("%.1fM rows written", (l->rows + l->tx_rows) / 1000000.0);
	return 0;

}

static int
checkpoint_write_tuple(struct xlog *l, uint32_t space_id, uint32_t group_id,
		       const char *data, uint32_t size)
{
	struct request_replace_body body;
	body.m_body = 0x82; /* map of two elements. */
	body.k_space_id = IPROTO_SPACE_ID;
	body.m_space_id = 0xce; /* uint32 */
	body.v_space_id = mp_bswap_u32(space_id);
	body.k_tuple = IPROTO_TUPLE;

	struct xrow_header row;
	memset(&row, 0, sizeof(struct xrow_header));
	row.type = IPROTO_INSERT;
	row.group_id = group_id;

	row.bodycnt = 2;
	row.body[0].iov_base = &body;
	row.body[0].iov_len = sizeof(body);
	row.body[1].iov_base = (char *)data;
	row.body[1].iov_len = size;
	return checkpoint_write_row(l, &row);
}

struct checkpoint_entry {
	uint32_t space_id;
	uint32_t group_id;
	struct snapshot_iterator *iterator;
	struct rlist link;
};

struct checkpoint {
	/**
	 * List of MemTX spaces to snapshot, with consistent
	 * read view iterators.
	 */
	struct rlist entries;
	struct cord cord;
	bool waiting_for_snap_thread;
	/** The vclock of the snapshot file. */
	struct vclock vclock;
	struct xdir dir;
	/**
	 * Do nothing, just touch the snapshot file - the
	 * checkpoint already exists.
	 */
	bool touch;
};

static struct checkpoint *
checkpoint_new(const char *snap_dirname, uint64_t snap_io_rate_limit)
{
	struct checkpoint *ckpt = malloc(sizeof(*ckpt));
	if (ckpt == NULL) {
		diag_set(OutOfMemory, sizeof(*ckpt), "malloc",
			 "struct checkpoint");
		return NULL;
	}
	rlist_create(&ckpt->entries);
	ckpt->waiting_for_snap_thread = false;
	struct xlog_opts opts = xlog_opts_default;
	opts.rate_limit = snap_io_rate_limit;
	opts.sync_interval = SNAP_SYNC_INTERVAL;
	opts.free_cache = true;
	xdir_create(&ckpt->dir, snap_dirname, SNAP, &INSTANCE_UUID, &opts);
	vclock_create(&ckpt->vclock);
	ckpt->touch = false;
	return ckpt;
}

static void
checkpoint_delete(struct checkpoint *ckpt)
{
	struct checkpoint_entry *entry, *tmp;
	rlist_foreach_entry_safe(entry, &ckpt->entries, link, tmp) {
		entry->iterator->free(entry->iterator);
		free(entry);
	}
	xdir_destroy(&ckpt->dir);
	free(ckpt);
}

static void
checkpoint_cancel(struct checkpoint *ckpt)
{
	/*
	 * Cancel the checkpoint thread if it's running and wait
	 * for it to terminate so as to eliminate the possibility
	 * of use-after-free.
	 */
	if (ckpt->waiting_for_snap_thread) {
		tt_pthread_cancel(ckpt->cord.id);
		tt_pthread_join(ckpt->cord.id, NULL);
	}
	checkpoint_delete(ckpt);
}

static int
checkpoint_add_space(struct space *sp, void *data)
{
	if (space_is_temporary(sp))
		return 0;
	if (!space_is_memtx(sp))
		return 0;
	struct index *pk = space_index(sp, 0);
	if (!pk)
		return 0;
	struct checkpoint *ckpt = (struct checkpoint *)data;
	struct checkpoint_entry *entry = malloc(sizeof(*entry));
	if (entry == NULL) {
		diag_set(OutOfMemory, sizeof(*entry),
			 "malloc", "struct checkpoint_entry");
		return -1;
	}
	rlist_add_tail_entry(&ckpt->entries, entry, link);

	entry->space_id = space_id(sp);
	entry->group_id = space_group_id(sp);
	entry->iterator = index_create_snapshot_iterator(pk);
	if (entry->iterator == NULL)
		return -1;

	return 0;
};

static int
checkpoint_f(va_list ap)
{
	struct checkpoint *ckpt = va_arg(ap, struct checkpoint *);

	if (ckpt->touch) {
		if (xdir_touch_xlog(&ckpt->dir, &ckpt->vclock) == 0)
			return 0;
		/*
		 * Failed to touch an existing snapshot, create
		 * a new one.
		 */
		ckpt->touch = false;
	}

	struct xlog snap;
	if (xdir_create_xlog(&ckpt->dir, &snap, &ckpt->vclock) != 0)
		return -1;

	say_info("saving snapshot `%s'", snap.filename);
	ERROR_INJECT_SLEEP(ERRINJ_SNAP_WRITE_DELAY);
	struct checkpoint_entry *entry;
	rlist_foreach_entry(entry, &ckpt->entries, link) {
		int rc;
		uint32_t size;
		const char *data;
		struct snapshot_iterator *it = entry->iterator;
		while ((rc = it->next(it, &data, &size)) == 0 && data != NULL) {
			if (checkpoint_write_tuple(&snap, entry->space_id,
					entry->group_id, data, size) != 0)
				goto fail;
		}
		if (rc != 0)
			goto fail;
	}
	if (xlog_flush(&snap) < 0)
		goto fail;

	xlog_close(&snap, false);
	say_info("done");
	return 0;
fail:
	xlog_close(&snap, false);
	return -1;
}

static int
memtx_engine_begin_checkpoint(struct engine *engine)
{
	struct memtx_engine *memtx = (struct memtx_engine *)engine;

	assert(memtx->checkpoint == NULL);
	memtx->checkpoint = checkpoint_new(memtx->snap_dir.dirname,
					   memtx->snap_io_rate_limit);
	if (memtx->checkpoint == NULL)
		return -1;

	if (space_foreach(checkpoint_add_space, memtx->checkpoint) != 0) {
		checkpoint_delete(memtx->checkpoint);
		memtx->checkpoint = NULL;
		return -1;
	}

	/* increment snapshot version; set tuple deletion to delayed mode */
	memtx->snapshot_version++;
	small_alloc_setopt(&memtx->alloc, SMALL_DELAYED_FREE_MODE, true);
	return 0;
}

static int
memtx_engine_wait_checkpoint(struct engine *engine,
			     const struct vclock *vclock)
{
	struct memtx_engine *memtx = (struct memtx_engine *)engine;

	assert(memtx->checkpoint != NULL);
	/*
	 * If a snapshot already exists, do not create a new one.
	 */
	struct vclock last;
	if (xdir_last_vclock(&memtx->snap_dir, &last) >= 0 &&
	    vclock_compare(&last, vclock) == 0) {
		memtx->checkpoint->touch = true;
	}
	vclock_copy(&memtx->checkpoint->vclock, vclock);

	if (cord_costart(&memtx->checkpoint->cord, "snapshot",
			 checkpoint_f, memtx->checkpoint)) {
		return -1;
	}
	memtx->checkpoint->waiting_for_snap_thread = true;

	/* wait for memtx-part snapshot completion */
	int result = cord_cojoin(&memtx->checkpoint->cord);
	if (result != 0)
		diag_log();

	memtx->checkpoint->waiting_for_snap_thread = false;
	return result;
}

/**
 * Called after checkpointing is complete to free indexes dropped
 * while checkpointing was in progress, see memtx_engine_run_gc().
 */
static void
memtx_engine_gc_after_checkpoint(struct memtx_engine *memtx)
{
	struct memtx_gc_task *task, *next;
	stailq_foreach_entry_safe(task, next, &memtx->gc_to_free, link)
		task->vtab->free(task);
	stailq_create(&memtx->gc_to_free);
}

static void
memtx_engine_commit_checkpoint(struct engine *engine,
			       const struct vclock *vclock)
{
	(void) vclock;
	struct memtx_engine *memtx = (struct memtx_engine *)engine;

	/* beginCheckpoint() must have been done */
	assert(memtx->checkpoint != NULL);
	/* waitCheckpoint() must have been done. */
	assert(!memtx->checkpoint->waiting_for_snap_thread);

	small_alloc_setopt(&memtx->alloc, SMALL_DELAYED_FREE_MODE, false);

	if (!memtx->checkpoint->touch) {
		int64_t lsn = vclock_sum(&memtx->checkpoint->vclock);
		struct xdir *dir = &memtx->checkpoint->dir;
		/* rename snapshot on completion */
		char to[PATH_MAX];
		snprintf(to, sizeof(to), "%s",
			 xdir_format_filename(dir, lsn, NONE));
		const char *from = xdir_format_filename(dir, lsn, INPROGRESS);
		ERROR_INJECT_YIELD(ERRINJ_SNAP_COMMIT_DELAY);
		int rc = coio_rename(from, to);
		if (rc != 0)
			panic("can't rename .snap.inprogress");
	}

	struct vclock last;
	if (xdir_last_vclock(&memtx->snap_dir, &last) < 0 ||
	    vclock_compare(&last, vclock) != 0) {
		/* Add the new checkpoint to the set. */
		xdir_add_vclock(&memtx->snap_dir, &memtx->checkpoint->vclock);
	}

	checkpoint_delete(memtx->checkpoint);
	memtx->checkpoint = NULL;

	memtx_engine_gc_after_checkpoint(memtx);
}

static void
memtx_engine_abort_checkpoint(struct engine *engine)
{
	struct memtx_engine *memtx = (struct memtx_engine *)engine;

	/**
	 * An error in the other engine's first phase.
	 */
	if (memtx->checkpoint->waiting_for_snap_thread) {
		/* wait for memtx-part snapshot completion */
		if (cord_cojoin(&memtx->checkpoint->cord) != 0)
			diag_log();
		memtx->checkpoint->waiting_for_snap_thread = false;
	}

	small_alloc_setopt(&memtx->alloc, SMALL_DELAYED_FREE_MODE, false);

	/** Remove garbage .inprogress file. */
	const char *filename =
		xdir_format_filename(&memtx->checkpoint->dir,
				     vclock_sum(&memtx->checkpoint->vclock),
				     INPROGRESS);
	(void) coio_unlink(filename);

	checkpoint_delete(memtx->checkpoint);
	memtx->checkpoint = NULL;
}

static void
memtx_engine_collect_garbage(struct engine *engine, const struct vclock *vclock)
{
	struct memtx_engine *memtx = (struct memtx_engine *)engine;
	xdir_collect_garbage(&memtx->snap_dir, vclock_sum(vclock),
			     XDIR_GC_ASYNC);
}

static int
memtx_engine_backup(struct engine *engine, const struct vclock *vclock,
		    engine_backup_cb cb, void *cb_arg)
{
	struct memtx_engine *memtx = (struct memtx_engine *)engine;
	const char *filename = xdir_format_filename(&memtx->snap_dir,
						    vclock_sum(vclock), NONE);
	return cb(filename, cb_arg);
}

/** Used to pass arguments to memtx_initial_join_f */
struct memtx_join_arg {
	const char *snap_dirname;
	int64_t checkpoint_lsn;
	struct xstream *stream;
};

/**
 * Invoked from a thread to feed snapshot rows.
 */
static int
memtx_initial_join_f(va_list ap)
{
	struct memtx_join_arg *arg = va_arg(ap, struct memtx_join_arg *);
	const char *snap_dirname = arg->snap_dirname;
	int64_t checkpoint_lsn = arg->checkpoint_lsn;
	struct xstream *stream = arg->stream;

	struct xdir dir;
	/*
	 * snap_dirname and INSTANCE_UUID don't change after start,
	 * safe to use in another thread.
	 */
	xdir_create(&dir, snap_dirname, SNAP, &INSTANCE_UUID,
		    &xlog_opts_default);
	struct xlog_cursor cursor;
	int rc = xdir_open_cursor(&dir, checkpoint_lsn, &cursor);
	xdir_destroy(&dir);
	if (rc < 0)
		return -1;

	struct xrow_header row;
	while ((rc = xlog_cursor_next(&cursor, &row, true)) == 0) {
		rc = xstream_write(stream, &row);
		if (rc < 0)
			break;
	}
	xlog_cursor_close(&cursor, false);
	if (rc < 0)
		return -1;

	/**
	 * We should never try to read snapshots with no EOF
	 * marker - such snapshots are very likely corrupted and
	 * should not be trusted.
	 */
	/* TODO: replace panic with diag_set() */
	if (!xlog_cursor_is_eof(&cursor))
		panic("snapshot `%s' has no EOF marker", cursor.name);
	return 0;
}

static int
memtx_engine_join(struct engine *engine, const struct vclock *vclock,
		  struct xstream *stream)
{
	struct memtx_engine *memtx = (struct memtx_engine *)engine;

	/*
	 * cord_costart() passes only void * pointer as an argument.
	 */
	struct memtx_join_arg arg = {
		/* .snap_dirname   = */ memtx->snap_dir.dirname,
		/* .checkpoint_lsn = */ vclock_sum(vclock),
		/* .stream         = */ stream
	};

	/* Send snapshot using a thread */
	struct cord cord;
	cord_costart(&cord, "initial_join", memtx_initial_join_f, &arg);
	return cord_cojoin(&cord);
}

static int
small_stats_noop_cb(const struct mempool_stats *stats, void *cb_ctx)
{
	(void)stats;
	(void)cb_ctx;
	return 0;
}

static void
memtx_engine_memory_stat(struct engine *engine, struct engine_memory_stat *stat)
{
	struct memtx_engine *memtx = (struct memtx_engine *)engine;
	struct small_stats data_stats;
	struct mempool_stats index_stats;
	mempool_stats(&memtx->index_extent_pool, &index_stats);
	small_stats(&memtx->alloc, &data_stats, small_stats_noop_cb, NULL);
	stat->data += data_stats.used;
	stat->index += index_stats.totals.used;
}

static const struct engine_vtab memtx_engine_vtab = {
	/* .shutdown = */ memtx_engine_shutdown,
	/* .create_space = */ memtx_engine_create_space,
	/* .join = */ memtx_engine_join,
	/* .begin = */ memtx_engine_begin,
	/* .begin_statement = */ generic_engine_begin_statement,
	/* .prepare = */ generic_engine_prepare,
	/* .commit = */ generic_engine_commit,
	/* .rollback_statement = */ memtx_engine_rollback_statement,
	/* .rollback = */ generic_engine_rollback,
	/* .switch_to_ro = */ generic_engine_switch_to_ro,
	/* .bootstrap = */ memtx_engine_bootstrap,
	/* .begin_initial_recovery = */ memtx_engine_begin_initial_recovery,
	/* .begin_final_recovery = */ memtx_engine_begin_final_recovery,
	/* .end_recovery = */ memtx_engine_end_recovery,
	/* .begin_checkpoint = */ memtx_engine_begin_checkpoint,
	/* .wait_checkpoint = */ memtx_engine_wait_checkpoint,
	/* .commit_checkpoint = */ memtx_engine_commit_checkpoint,
	/* .abort_checkpoint = */ memtx_engine_abort_checkpoint,
	/* .collect_garbage = */ memtx_engine_collect_garbage,
	/* .backup = */ memtx_engine_backup,
	/* .memory_stat = */ memtx_engine_memory_stat,
	/* .reset_stat = */ generic_engine_reset_stat,
	/* .check_space_def = */ generic_engine_check_space_def,
};

/**
 * Run one iteration of garbage collection. Set @stop if
 * there is no more objects to free.
 */
static void
memtx_engine_run_gc(struct memtx_engine *memtx, bool *stop)
{
	*stop = stailq_empty(&memtx->gc_queue);
	if (*stop)
		return;

	struct memtx_gc_task *task = stailq_first_entry(&memtx->gc_queue,
					struct memtx_gc_task, link);
	bool task_done;
	task->vtab->run(task, &task_done);
	if (task_done) {
		stailq_shift(&memtx->gc_queue);
		/*
		 * If checkpointing is in progress, the index may be
		 * used by the checkpoint thread so we postpone freeing
		 * until checkpointing is complete.
		 */
		if (memtx->checkpoint == NULL)
			task->vtab->free(task);
		else
			stailq_add_entry(&memtx->gc_to_free, task, link);
	}
}

static int
memtx_engine_gc_f(va_list va)
{
	struct memtx_engine *memtx = va_arg(va, struct memtx_engine *);
	while (!fiber_is_cancelled()) {
		bool stop;
		ERROR_INJECT_YIELD(ERRINJ_MEMTX_DELAY_GC);
		memtx_engine_run_gc(memtx, &stop);
		if (stop) {
			fiber_yield_timeout(TIMEOUT_INFINITY);
			continue;
		}
		/*
		 * Yield after each iteration so as not to block
		 * tx thread for too long.
		 */
		fiber_sleep(0);
	}
	return 0;
}

struct memtx_engine *
memtx_engine_new(const char *snap_dirname, bool force_recovery,
		 uint64_t tuple_arena_max_size, uint32_t objsize_min,
		 bool dontdump, float alloc_factor)
{
	struct memtx_engine *memtx = calloc(1, sizeof(*memtx));
	if (memtx == NULL) {
		diag_set(OutOfMemory, sizeof(*memtx),
			 "malloc", "struct memtx_engine");
		return NULL;
	}

	xdir_create(&memtx->snap_dir, snap_dirname, SNAP, &INSTANCE_UUID,
		    &xlog_opts_default);
	memtx->snap_dir.force_recovery = force_recovery;

	if (xdir_scan(&memtx->snap_dir) != 0)
		goto fail;

	/*
	 * To check if the instance needs to be rebootstrapped, we
	 * need to connect it to remote peers before proceeding to
	 * local recovery. In order to do that, we have to start
	 * listening for incoming connections, because one of remote
	 * peers may be self. This, in turn, requires us to know the
	 * instance UUID, as it is a part of a greeting message.
	 * So if the local directory isn't empty, read the snapshot
	 * signature right now to initialize the instance UUID.
	 */
	int64_t snap_signature = xdir_last_vclock(&memtx->snap_dir, NULL);
	if (snap_signature >= 0) {
		struct xlog_cursor cursor;
		if (xdir_open_cursor(&memtx->snap_dir,
				     snap_signature, &cursor) != 0)
			goto fail;
		INSTANCE_UUID = cursor.meta.instance_uuid;
		xlog_cursor_close(&cursor, false);
	}

	/* Apprise the garbage collector of available checkpoints. */
	for (struct vclock *vclock = vclockset_first(&memtx->snap_dir.index);
	     vclock != NULL;
	     vclock = vclockset_next(&memtx->snap_dir.index, vclock)) {
		gc_add_checkpoint(vclock);
	}

	stailq_create(&memtx->gc_queue);
	stailq_create(&memtx->gc_to_free);
	memtx->gc_fiber = fiber_new("memtx.gc", memtx_engine_gc_f);
	if (memtx->gc_fiber == NULL)
		goto fail;

	/* Apply lowest allowed objsize bound. */
	if (objsize_min < OBJSIZE_MIN)
		objsize_min = OBJSIZE_MIN;

	/* Initialize tuple allocator. */
	quota_init(&memtx->quota, tuple_arena_max_size);
	tuple_arena_create(&memtx->arena, &memtx->quota, tuple_arena_max_size,
			   SLAB_SIZE, dontdump, "memtx");
	slab_cache_create(&memtx->slab_cache, &memtx->arena);
	small_alloc_create(&memtx->alloc, &memtx->slab_cache,
			   objsize_min, alloc_factor);

	/* Initialize index extent allocator. */
	slab_cache_create(&memtx->index_slab_cache, &memtx->arena);
	mempool_create(&memtx->index_extent_pool, &memtx->index_slab_cache,
		       MEMTX_EXTENT_SIZE);
	mempool_create(&memtx->iterator_pool, cord_slab_cache(),
		       MEMTX_ITERATOR_SIZE);
	memtx->num_reserved_extents = 0;
	memtx->reserved_extents = NULL;

	memtx->state = MEMTX_INITIALIZED;
	memtx->max_tuple_size = MAX_TUPLE_SIZE;
	memtx->force_recovery = force_recovery;

	memtx->base.vtab = &memtx_engine_vtab;
	memtx->base.name = "memtx";

	fiber_start(memtx->gc_fiber, memtx);
	return memtx;
fail:
	xdir_destroy(&memtx->snap_dir);
	free(memtx);
	return NULL;
}

void
memtx_engine_schedule_gc(struct memtx_engine *memtx,
			 struct memtx_gc_task *task)
{
	stailq_add_tail_entry(&memtx->gc_queue, task, link);
	fiber_wakeup(memtx->gc_fiber);
}

void
memtx_engine_set_snap_io_rate_limit(struct memtx_engine *memtx, double limit)
{
	memtx->snap_io_rate_limit = limit * 1024 * 1024;
}

int
memtx_engine_set_memory(struct memtx_engine *memtx, size_t size)
{
	if (size < quota_total(&memtx->quota)) {
		diag_set(ClientError, ER_CFG, "memtx_memory",
			 "cannot decrease memory size at runtime");
		return -1;
	}
	quota_set(&memtx->quota, size);
	return 0;
}

void
memtx_engine_set_max_tuple_size(struct memtx_engine *memtx, size_t max_size)
{
	memtx->max_tuple_size = max_size;
}

struct tuple *
memtx_tuple_new(struct tuple_format *format, const char *data, const char *end)
{
	struct memtx_engine *memtx = (struct memtx_engine *)format->engine;
	assert(mp_typeof(*data) == MP_ARRAY);
	struct tuple *tuple = NULL;
	struct region *region = &fiber()->gc;
	size_t region_svp = region_used(region);
	struct field_map_builder builder;
	if (tuple_field_map_create(format, data, true, &builder) != 0)
		goto end;
	uint32_t field_map_size = field_map_build_size(&builder);

	size_t tuple_len = end - data;
	size_t total = sizeof(struct memtx_tuple) + field_map_size + tuple_len;

	ERROR_INJECT(ERRINJ_TUPLE_ALLOC, {
		diag_set(OutOfMemory, total, "slab allocator", "memtx_tuple");
		goto end;
	});
	if (unlikely(total > memtx->max_tuple_size)) {
		diag_set(ClientError, ER_MEMTX_MAX_TUPLE_SIZE, total);
		error_log(diag_last_error(diag_get()));
		goto end;
	}

	struct memtx_tuple *memtx_tuple;
	while ((memtx_tuple = smalloc(&memtx->alloc, total)) == NULL) {
		bool stop;
		memtx_engine_run_gc(memtx, &stop);
		if (stop)
			break;
	}
	if (memtx_tuple == NULL) {
		diag_set(OutOfMemory, total, "slab allocator", "memtx_tuple");
		goto end;
	}
	tuple = &memtx_tuple->base;
	tuple->refs = 0;
	memtx_tuple->version = memtx->snapshot_version;
	assert(tuple_len <= UINT32_MAX); /* bsize is UINT32_MAX */
	tuple->bsize = tuple_len;
	tuple->format_id = tuple_format_id(format);
	tuple_format_ref(format);
	/*
	 * Data offset is calculated from the begin of the struct
	 * tuple base, not from memtx_tuple, because the struct
	 * tuple is not the first field of the memtx_tuple.
	 */
	tuple->data_offset = sizeof(struct tuple) + field_map_size;
	char *raw = (char *) tuple + tuple->data_offset;
	field_map_build(&builder, raw - field_map_size);
	memcpy(raw, data, tuple_len);
	say_debug("%s(%zu) = %p", __func__, tuple_len, memtx_tuple);
end:
	region_truncate(region, region_svp);
	return tuple;
}

void
memtx_tuple_delete(struct tuple_format *format, struct tuple *tuple)
{
	struct memtx_engine *memtx = (struct memtx_engine *)format->engine;
	say_debug("%s(%p)", __func__, tuple);
	assert(tuple->refs == 0);
	tuple_format_unref(format);
	struct memtx_tuple *memtx_tuple =
		container_of(tuple, struct memtx_tuple, base);
	size_t total = tuple_size(tuple) + offsetof(struct memtx_tuple, base);
	if (memtx->alloc.free_mode != SMALL_DELAYED_FREE ||
	    memtx_tuple->version == memtx->snapshot_version ||
	    format->is_temporary)
		smfree(&memtx->alloc, memtx_tuple, total);
	else
		smfree_delayed(&memtx->alloc, memtx_tuple, total);
}

void
metmx_tuple_chunk_delete(struct tuple_format *format, const char *data)
{
	struct memtx_engine *memtx = (struct memtx_engine *)format->engine;
	struct tuple_chunk *tuple_chunk =
		container_of((typeof(tuple_chunk->data) *)data,
			     struct tuple_chunk, data);
	uint32_t sz = tuple_chunk_sz(tuple_chunk->data_sz);
	smfree(&memtx->alloc, tuple_chunk, sz);
}

const char *
memtx_tuple_chunk_new(struct tuple_format *format, struct tuple *tuple,
		      const char *data, uint32_t data_sz)
{
	struct memtx_engine *memtx = (struct memtx_engine *)format->engine;
	uint32_t sz = tuple_chunk_sz(data_sz);
	struct tuple_chunk *tuple_chunk =
		(struct tuple_chunk *) smalloc(&memtx->alloc, sz);
	if (tuple == NULL) {
		diag_set(OutOfMemory, sz, "smalloc", "tuple");
		return NULL;
	}
	tuple_chunk->data_sz = data_sz;
	memcpy(tuple_chunk->data, data, data_sz);
	return tuple_chunk->data;
}

struct tuple_format_vtab memtx_tuple_format_vtab = {
	memtx_tuple_delete,
	memtx_tuple_new,
	metmx_tuple_chunk_delete,
	memtx_tuple_chunk_new,
};

/**
 * Allocate a block of size MEMTX_EXTENT_SIZE for memtx index
 */
void *
memtx_index_extent_alloc(void *ctx)
{
	struct memtx_engine *memtx = (struct memtx_engine *)ctx;
	if (memtx->reserved_extents) {
		assert(memtx->num_reserved_extents > 0);
		memtx->num_reserved_extents--;
		void *result = memtx->reserved_extents;
		memtx->reserved_extents = *(void **)memtx->reserved_extents;
		return result;
	}
	ERROR_INJECT(ERRINJ_INDEX_ALLOC, {
		/* same error as in mempool_alloc */
		diag_set(OutOfMemory, MEMTX_EXTENT_SIZE,
			 "mempool", "new slab");
		return NULL;
	});
	void *ret;
	while ((ret = mempool_alloc(&memtx->index_extent_pool)) == NULL) {
		bool stop;
		memtx_engine_run_gc(memtx, &stop);
		if (stop)
			break;
	}
	if (ret == NULL)
		diag_set(OutOfMemory, MEMTX_EXTENT_SIZE,
			 "mempool", "new slab");
	return ret;
}

/**
 * Free a block previously allocated by memtx_index_extent_alloc
 */
void
memtx_index_extent_free(void *ctx, void *extent)
{
	struct memtx_engine *memtx = (struct memtx_engine *)ctx;
	return mempool_free(&memtx->index_extent_pool, extent);
}

/**
 * Reserve num extents in pool.
 * Ensure that next num extent_alloc will succeed w/o an error
 */
int
memtx_index_extent_reserve(struct memtx_engine *memtx, int num)
{
	ERROR_INJECT(ERRINJ_INDEX_ALLOC, {
		/* same error as in mempool_alloc */
		diag_set(OutOfMemory, MEMTX_EXTENT_SIZE,
			 "mempool", "new slab");
		return -1;
	});
	struct mempool *pool = &memtx->index_extent_pool;
	while (memtx->num_reserved_extents < num) {
		void *ext;
		while ((ext = mempool_alloc(pool)) == NULL) {
			bool stop;
			memtx_engine_run_gc(memtx, &stop);
			if (stop)
				break;
		}
		if (ext == NULL) {
			diag_set(OutOfMemory, MEMTX_EXTENT_SIZE,
				 "mempool", "new slab");
			return -1;
		}
		*(void **)ext = memtx->reserved_extents;
		memtx->reserved_extents = ext;
		memtx->num_reserved_extents++;
	}
	return 0;
}

bool
memtx_index_def_change_requires_rebuild(struct index *index,
					const struct index_def *new_def)
{
	struct index_def *old_def = index->def;

	assert(old_def->iid == new_def->iid);
	assert(old_def->space_id == new_def->space_id);

	if (old_def->type != new_def->type)
		return true;
	if (!old_def->opts.is_unique && new_def->opts.is_unique)
		return true;
	if (old_def->opts.func_id != new_def->opts.func_id)
		return true;

	const struct key_def *old_cmp_def, *new_cmp_def;
	if (index_depends_on_pk(index)) {
		old_cmp_def = old_def->cmp_def;
		new_cmp_def = new_def->cmp_def;
	} else {
		old_cmp_def = old_def->key_def;
		new_cmp_def = new_def->key_def;
	}

	/*
	 * Compatibility of field types is verified by CheckSpaceFormat
	 * so it suffices to check that the new key definition indexes
	 * the same set of fields in the same order.
	 */
	if (old_cmp_def->part_count != new_cmp_def->part_count)
		return true;

	for (uint32_t i = 0; i < new_cmp_def->part_count; i++) {
		const struct key_part *old_part = &old_cmp_def->parts[i];
		const struct key_part *new_part = &new_cmp_def->parts[i];
		if (old_part->fieldno != new_part->fieldno)
			return true;
		if (old_part->coll != new_part->coll)
			return true;
		if (json_path_cmp(old_part->path, old_part->path_len,
				  new_part->path, new_part->path_len,
				  TUPLE_INDEX_BASE) != 0)
			return true;
	}
	assert(old_cmp_def->is_multikey == new_cmp_def->is_multikey);
	return false;
}
