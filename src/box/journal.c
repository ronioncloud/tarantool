/*
 * Copyright 2010-2017, Tarantool AUTHORS, please see AUTHORS file.
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
#include "journal.h"
#include <small/region.h>
#include <diag.h>

struct journal *current_journal = NULL;

struct journal_entry *
journal_entry_new(size_t n_rows, struct region *region,
		  journal_write_async_f write_async_cb,
		  void *complete_data)
{
	struct journal_entry *entry;

	size_t size = (sizeof(struct journal_entry) +
		       sizeof(entry->rows[0]) * n_rows);

	entry = region_aligned_alloc(region, size,
				     alignof(struct journal_entry));
	if (entry == NULL) {
		diag_set(OutOfMemory, size, "region", "struct journal_entry");
		return NULL;
	}

	journal_entry_create(entry, n_rows, 0, write_async_cb,
			     complete_data);
	return entry;
}

struct journal_queue_entry {
	/** The fiber waiting for queue space to free. */
	struct fiber *fiber;
	/** Whether the fiber should be waken up regardless of queue size. */
	bool is_ready;
	/** A link in all waiting fibers list. */
	struct rlist in_queue;
};

/**
 * Wake up the next waiter in journal queue.
 */
static inline void
journal_queue_wakeup_next(struct rlist *link, bool force_ready)
{
	/* Empty queue or last entry in queue. */
	if (link == rlist_last(&current_journal->waiters)) {
		current_journal->queue_is_awake = false;
		return;
	}
	/*
	 * When the queue isn't forcefully emptied, no need to wake everyone
	 * else up until there's some free space.
	 */
	if (!force_ready && journal_queue_is_full()) {
		current_journal->queue_is_awake = false;
		return;
	}
	struct journal_queue_entry *e = rlist_entry(rlist_next(link), typeof(*e),
						    in_queue);
	e->is_ready = force_ready;
	fiber_wakeup(e->fiber);
}

void
journal_queue_wakeup(bool force_ready)
{
	assert(!rlist_empty(&current_journal->waiters));
	if (current_journal->queue_is_awake)
		return;
	current_journal->queue_is_awake = true;
	journal_queue_wakeup_next(&current_journal->waiters, force_ready);
}

void
journal_wait_queue(void)
{
	struct journal_queue_entry entry = {
		.fiber = fiber(),
		.is_ready = false,
	};
	rlist_add_tail_entry(&current_journal->waiters, &entry, in_queue);
	/*
	 * Will be waken up by either queue emptying or a synchronous write.
	 */
	while (journal_queue_is_full() && !entry.is_ready)
		fiber_yield();

	journal_queue_wakeup_next(&entry.in_queue, entry.is_ready);
	assert(&entry.in_queue == rlist_first(&current_journal->waiters));
	rlist_del(&entry.in_queue);
}
