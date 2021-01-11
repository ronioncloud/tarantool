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
#include "func.h"
#include "fiber.h"
#include "trivia/config.h"
#include "lua/utils.h"
#include "lua/call.h"
#include "diag.h"
#include "port.h"
#include "schema.h"
#include "session.h"

struct func_c {
	/** Function object base class. */
	struct func base;
	/** C function module symbol. */
	struct module_sym mod_sym;
};

static struct func *
func_c_new(struct func_def *def);

/** Construct a SQL builtin function object. */
extern struct func *
func_sql_builtin_new(struct func_def *def);

struct func *
func_new(struct func_def *def)
{
	struct func *func;
	switch (def->language) {
	case FUNC_LANGUAGE_C:
		func = func_c_new(def);
		break;
	case FUNC_LANGUAGE_LUA:
		func = func_lua_new(def);
		break;
	case FUNC_LANGUAGE_SQL_BUILTIN:
		func = func_sql_builtin_new(def);
		break;
	default:
		unreachable();
	}
	if (func == NULL)
		return NULL;
	func->def = def;
	/** Nobody has access to the function but the owner. */
	memset(func->access, 0, sizeof(func->access));
	/*
	 * Do not initialize the privilege cache right away since
	 * when loading up a function definition during recovery,
	 * user cache may not be filled up yet (space _user is
	 * recovered after space _func), so no user cache entry
	 * may exist yet for such user.  The cache will be filled
	 * up on demand upon first access.
	 *
	 * Later on consistency of the cache is ensured by DDL
	 * checks (see user_has_data()).
	 */
	credentials_create_empty(&func->owner_credentials);
	return func;
}

static struct func_vtab func_c_vtab;

static struct func *
func_c_new(MAYBE_UNUSED struct func_def *def)
{
	assert(def->language == FUNC_LANGUAGE_C);
	assert(def->body == NULL && !def->is_sandboxed);
	struct func_c *func = (struct func_c *) malloc(sizeof(struct func_c));
	if (func == NULL) {
		diag_set(OutOfMemory, sizeof(*func), "malloc", "func");
		return NULL;
	}
	func->base.vtab = &func_c_vtab;
	func->mod_sym.addr = NULL;
	func->mod_sym.module = NULL;
	func->mod_sym.name = def->name;
	return &func->base;
}

static void
func_c_destroy(struct func *base)
{
	assert(base->vtab == &func_c_vtab);
	assert(base != NULL && base->def->language == FUNC_LANGUAGE_C);
	struct func_c *func = (struct func_c *) base;
	module_sym_unload(&func->mod_sym);
	TRASH(base);
	free(func);
}

int
func_c_call(struct func *base, struct port *args, struct port *ret)
{
	assert(base->vtab == &func_c_vtab);
	assert(base != NULL && base->def->language == FUNC_LANGUAGE_C);

	struct func_c *func = (struct func_c *)base;
	return module_sym_call(&func->mod_sym, args, ret);
}

static struct func_vtab func_c_vtab = {
	.call = func_c_call,
	.destroy = func_c_destroy,
};

void
func_delete(struct func *func)
{
	struct func_def *def = func->def;
	credentials_destroy(&func->owner_credentials);
	func->vtab->destroy(func);
	free(def);
}

/** Check "EXECUTE" permissions for a given function. */
static int
func_access_check(struct func *func)
{
	struct credentials *credentials = effective_user();
	/*
	 * If the user has universal access, don't bother with
	 * checks. No special check for ADMIN user is necessary
	 * since ADMIN has universal access.
	 */
	if ((credentials->universal_access & (PRIV_X | PRIV_U)) ==
	    (PRIV_X | PRIV_U))
		return 0;
	user_access_t access = PRIV_X | PRIV_U;
	/* Check access for all functions. */
	access &= ~entity_access_get(SC_FUNCTION)[credentials->auth_token].effective;
	user_access_t func_access = access & ~credentials->universal_access;
	if ((func_access & PRIV_U) != 0 ||
	    (func->def->uid != credentials->uid &&
	     func_access & ~func->access[credentials->auth_token].effective)) {
		/* Access violation, report error. */
		struct user *user = user_find(credentials->uid);
		if (user != NULL) {
			diag_set(AccessDeniedError, priv_name(PRIV_X),
				 schema_object_name(SC_FUNCTION),
				 func->def->name, user->def->name);
		}
		return -1;
	}
	return 0;
}

int
func_call(struct func *base, struct port *args, struct port *ret)
{
	if (func_access_check(base) != 0)
		return -1;
	/**
	 * Change the current user id if the function is
	 * a set-definer-uid one. If the function is not
	 * defined, it's obviously not a setuid one.
	 */
	struct credentials *orig_credentials = NULL;
	if (base->def->setuid) {
		orig_credentials = effective_user();
		/* Remember and change the current user id. */
		if (credentials_is_empty(&base->owner_credentials)) {
			/*
			 * Fill the cache upon first access, since
			 * when func is created, no user may
			 * be around to fill it (recovery of
			 * system spaces from a snapshot).
			 */
			struct user *owner = user_find(base->def->uid);
			if (owner == NULL)
				return -1;
			credentials_reset(&base->owner_credentials, owner);
		}
		fiber_set_user(fiber(), &base->owner_credentials);
	}
	int rc = base->vtab->call(base, args, ret);
	/* Restore the original user */
	if (orig_credentials)
		fiber_set_user(fiber(), orig_credentials);
	return rc;
}
