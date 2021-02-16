/*
 * Copyright 2010-2021, Tarantool AUTHORS, please see AUTHORS file.
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
 * THIS SOFTWARE IS PROVIDED BY AUTHORS ``AS IS'' AND
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

#include "on_shutdown.h"

#include <stdlib.h>
#include <small/rlist.h>
#include <errno.h>

enum {
	/** Time to wait for the trigger finished */
	XTM_TRIGGER_WAIT_TIME = 3,
};

struct on_shutdown_trigger {
	struct trigger trigger;
	/** Shutdown trigger function */
	int (*handler)(void *);
	/** Trigger function argument */
	void *arg;
};

/**
 * Shutdown trigger list, not need mutex, because access is
 * avaliable only from tx thread
 */
static struct rlist on_shutdown_trigger_list =
	RLIST_HEAD_INITIALIZER(on_shutdown_trigger_list);

static int
trigger_commom_f(struct trigger *trigger, MAYBE_UNUSED void *event)
{
	struct on_shutdown_trigger *on_shutdown_trigger =
		container_of(trigger, struct on_shutdown_trigger, trigger);
	return on_shutdown_trigger->handler(on_shutdown_trigger->arg);
}

static int
on_shutdown_trigger_create(int (*handler)(void *), void *arg)
{
	struct on_shutdown_trigger *trigger = (struct on_shutdown_trigger *)
		malloc(sizeof(struct on_shutdown_trigger));
	if (trigger == NULL)
		return -1;
	trigger_create(&trigger->trigger, trigger_commom_f, NULL, NULL);
	trigger->handler = handler;
	trigger->arg = arg;
	trigger_add(&on_shutdown_trigger_list, &trigger->trigger);
	return 0;
}

/**
 * We must explicitly mark this function as used for the compiler,
 * because otherwise it will be removed from the executable file
 * during the lto build.
 */
API_EXPORT USED int
on_shutdown_trigger_replace(void *arg, int (*new_handler)(void *),
			    int (*old_handler)(void *))
{
	struct trigger *trigger, *tmp;
	if (old_handler == NULL) {
		if (new_handler != NULL) {
			return on_shutdown_trigger_create(new_handler, arg);
		} else {
			/*
			 * Invalid function params, old_handler or new_handler
			 * must be set
			 */
			errno = EINVAL;
			return -1;
		}
	}

	rlist_foreach_entry_safe(trigger, &on_shutdown_trigger_list,
				 link, tmp) {
		struct on_shutdown_trigger *on_shutdown_trigger =
			container_of(trigger, struct on_shutdown_trigger,
				     trigger);
		if (on_shutdown_trigger->handler == old_handler) {
			if (new_handler != NULL) {
				/*
				 * Change on_shutdown trigger handler, and arg
				 */
				on_shutdown_trigger->handler = new_handler;
				on_shutdown_trigger->arg = arg;
			} else {
				/*
				 * In case new_handler == NULL
				 * Remove old on_shutdown trigger and destroy it
				 */
				trigger_clear(trigger);
				free(on_shutdown_trigger);
			}
			return 0;
		}
	}

	/*
	 * Here we are in case when we not find on_shutdown trigger,
	 * which we want to destroy return -1.
	 */
	errno = EINVAL;
	return -1;
}

int
run_on_shutdown_triggers(MAYBE_UNUSED struct trigger *trigger,
			 void *event)
{
	trigger_fiber_run_reverse(&on_shutdown_trigger_list,
				  event, XTM_TRIGGER_WAIT_TIME);
	return 0;
}

