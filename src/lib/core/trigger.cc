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

#include "trigger.h"

#include "fiber.h"

static int
trigger_fiber_f(va_list ap)
{
	struct trigger *trigger = va_arg(ap, struct trigger *);
	void *event = va_arg(ap, void *);
	return trigger->run(trigger, event);
}

int
trigger_run(struct rlist *list, void *event)
{
	struct trigger *trigger, *tmp;
	rlist_foreach_entry_safe(trigger, list, link, tmp)
		if (trigger->run(trigger, event) != 0)
			return -1;
	return 0;
}

int
trigger_run_reverse(struct rlist *list, void *event)
{
	struct trigger *trigger, *tmp;
	rlist_foreach_entry_safe_reverse(trigger, list, link, tmp)
		if (trigger->run(trigger, event) != 0)
			return -1;
	return 0;
}

void
trigger_fiber_run_reverse(struct rlist *list, void *event, double timeout)
{
	struct trigger *trigger, *tmp;
	unsigned num = 1;

	rlist_foreach_entry_safe_reverse(trigger, list, link, tmp) {
		char name[FIBER_NAME_INLINE];
		snprintf(name, FIBER_NAME_INLINE, "trigger_fiber%d", num++);
		trigger->fiber = fiber_new(name, trigger_fiber_f);
		if (trigger->fiber != NULL) {
			fiber_set_joinable(trigger->fiber, true);
			fiber_start(trigger->fiber, trigger, event);
		} else {
			diag_log();
			diag_clear(diag_get());
			/*
			 * In case we can't create fiber,
			 * run trigger here immediately.
			 */
			if (trigger->run(trigger, event) != 0 &&
			    ! diag_is_empty(diag_get())) {
				diag_log();
				diag_clear(diag_get());
			}
		}
	}

	/*
	 * Waiting for all triggers completion
	 */
	rlist_foreach_entry_safe_reverse(trigger, list, link, tmp) {
		if (trigger->fiber != NULL &&
		    fiber_join_timeout(trigger->fiber, timeout) != 0 &&
		    ! diag_is_empty(diag_get())) {
			diag_log();
			diag_clear(diag_get());
		}
	}
}
