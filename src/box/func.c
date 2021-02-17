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
#include "assoc.h"
#include "lua/utils.h"
#include "lua/call.h"
#include "lua/cmod.h"
#include "error.h"
#include "errinj.h"
#include "diag.h"
#include "port.h"
#include "schema.h"
#include "session.h"
#include "libeio/eio.h"
#include <fcntl.h>
#include <dlfcn.h>

/**
 * Parsed symbol and package names.
 */
struct func_name {
	/** Null-terminated symbol name, e.g. "func" for "mod.submod.func" */
	const char *sym;
	/** Package name, e.g. "mod.submod" for "mod.submod.func" */
	const char *package;
	/** A pointer to the last character in ->package + 1 */
	const char *package_end;
};

struct func_c {
	/** Function object base class. */
	struct func base;
	/**
	 * Anchor for module membership.
	 */
	struct rlist item;
	/**
	 * For C functions, the body of the function.
	 */
	box_function_f func;
	/**
	 * Each stored function keeps a handle to the
	 * dynamic library for the C callback.
	 */
	struct module *module;
};

/***
 * Split function name to symbol and package names.
 * For example, str = foo.bar.baz => sym = baz, package = foo.bar
 * @param str function name, e.g. "module.submodule.function".
 * @param[out] name parsed symbol and package names.
 */
static void
func_split_name(const char *str, struct func_name *name)
{
	name->package = str;
	name->package_end = strrchr(str, '.');
	if (name->package_end != NULL) {
		/* module.submodule.function => module.submodule, function */
		name->sym = name->package_end + 1; /* skip '.' */
	} else {
		/* package == function => function, function */
		name->sym = name->package;
		name->package_end = str + strlen(str);
	}
}

static struct mh_strnptr_t *modules = NULL;

static void
module_gc(struct module *module);

int
module_init(void)
{
	modules = mh_strnptr_new();
	if (modules == NULL) {
		diag_set(OutOfMemory, sizeof(*modules), "malloc",
			  "modules hash table");
		return -1;
	}
	return 0;
}

void
module_free(void)
{
	while (mh_size(modules) > 0) {
		mh_int_t i = mh_first(modules);
		struct module *module =
			(struct module *) mh_strnptr_node(modules, i)->val;
		/* Can't delete modules if they have active calls */
		module_gc(module);
	}
	mh_strnptr_delete(modules);
}

/**
 * Look up a module in the modules cache.
 */
static struct module *
module_cache_find(const char *name, const char *name_end)
{
	mh_int_t i = mh_strnptr_find_inp(modules, name, name_end - name);
	if (i == mh_end(modules))
		return NULL;
	return (struct module *)mh_strnptr_node(modules, i)->val;
}

/**
 * Save module to the module cache.
 */
static inline int
module_cache_put(struct module *module)
{
	const char *package = module->cmod->package;
	size_t package_len = module->cmod->package_len;
	const struct mh_strnptr_node_t strnode = {
		.str	= package,
		.len	= package_len,
		.hash	= mh_strn_hash(package, package_len),
		.val	= module,
	};
	if (mh_strnptr_put(modules, &strnode, NULL, NULL) == mh_end(modules)) {
		diag_set(OutOfMemory, sizeof(strnode), "malloc", "modules");
		return -1;
	}
	return 0;
}

/**
 * Update the module cache.
 */
static void
module_cache_update(struct module *module)
{
	const char *package = module->cmod->package;
	size_t package_len = module->cmod->package_len;
	mh_int_t i = mh_strnptr_find_inp(modules, package, package_len);
	if (i == mh_end(modules))
		panic("failed to update module cache");
	mh_strnptr_node(modules, i)->str = package;
	mh_strnptr_node(modules, i)->val = module;
}

/**
 * Delete a module from the module cache
 */
static void
module_cache_del(const char *name, const char *name_end)
{
	mh_int_t i = mh_strnptr_find_inp(modules, name, name_end - name);
	if (i == mh_end(modules))
		return;
	mh_strnptr_del(modules, i, NULL);
}

/**
 * Allocate a new module instance.
 */
static struct module *
module_new(struct cmod *cmod)
{
	struct module *module = malloc(sizeof(*module));
	if (module == NULL) {
		diag_set(OutOfMemory, sizeof(struct module),
			 "malloc", "struct module");
		return NULL;
	}

	rlist_create(&module->funcs);
	module->cmod = cmod;

	return module;
}

/**
 * Load a new DSO.
 */
static struct module *
module_load(const char *package, const char *package_end)
{
	char path[PATH_MAX];
	int package_len = package_end - package;

	if (cmod_find_package(package, package_len,
			      path, sizeof(path)) != 0) {
		return NULL;
	}

	struct cmod *cmod = cmod_new(package, package_len, path);
	if (cmod == NULL)
		return NULL;

	struct module *module = module_new(cmod);
	if (module == NULL) {
		cmod_unref(cmod);
		return NULL;
	}

	struct errinj *e = errinj(ERRINJ_DYN_MODULE_COUNT, ERRINJ_INT);
	if (e != NULL)
		++e->iparam;

	return module;
}

static void
module_delete(struct module *module)
{
	struct errinj *e = errinj(ERRINJ_DYN_MODULE_COUNT, ERRINJ_INT);
	if (e != NULL)
		--e->iparam;
	cmod_unref(module->cmod);
	TRASH(module);
	free(module);
}

/*
 * Check if a dso is unused and can be closed.
 */
static void
module_gc(struct module *module)
{
	if (rlist_empty(&module->funcs))
		module_delete(module);
}

/*
 * Import a function from the module.
 */
static box_function_f
module_sym(struct module *module, const char *name)
{
	void *handle = module->cmod->handle;
	box_function_f f = (box_function_f)dlsym(handle, name);
	if (f == NULL) {
		diag_set(ClientError, ER_LOAD_FUNCTION, name, dlerror());
		return NULL;
	}
	return f;
}

int
module_reload(const char *package, const char *package_end)
{
	struct module *old_module = module_cache_find(package, package_end);
	if (old_module == NULL) {
		diag_set(ClientError, ER_NO_SUCH_MODULE, package);
		return -1;
	}

	struct module *new_module = module_load(package, package_end);
	if (new_module == NULL)
		return -1;

	struct func_c *func, *tmp_func;
	rlist_foreach_entry_safe(func, &old_module->funcs, item, tmp_func) {
		struct func_name name;
		func_split_name(func->base.def->name, &name);
		func->func = module_sym(new_module, name.sym);
		if (func->func == NULL)
			goto restore;
		func->module = new_module;
		rlist_move(&new_module->funcs, &func->item);
	}
	module_cache_update(new_module);
	module_gc(old_module);
	return 0;
restore:
	/*
	 * Some old-dso func can't be load from new module, restore old
	 * functions.
	 */
	do {
		struct func_name name;
		func_split_name(func->base.def->name, &name);
		func->func = module_sym(old_module, name.sym);
		if (func->func == NULL) {
			/*
			 * Something strange was happen, an early loaden
			 * function was not found in an old dso.
			 */
			panic("Can't restore module function, "
			      "server state is inconsistent");
		}
		func->module = old_module;
		rlist_move(&old_module->funcs, &func->item);
	} while (func != rlist_first_entry(&old_module->funcs,
					   struct func_c, item));
	assert(rlist_empty(&new_module->funcs));
	module_delete(new_module);
	return -1;
}

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
	func->func = NULL;
	func->module = NULL;
	return &func->base;
}

static void
func_c_unload(struct func_c *func)
{
	if (func->module) {
		rlist_del(&func->item);
		if (rlist_empty(&func->module->funcs)) {
			struct func_name name;
			func_split_name(func->base.def->name, &name);
			module_cache_del(name.package, name.package_end);
		}
		module_gc(func->module);
	}
	func->module = NULL;
	func->func = NULL;
}

static void
func_c_destroy(struct func *base)
{
	assert(base->vtab == &func_c_vtab);
	assert(base != NULL && base->def->language == FUNC_LANGUAGE_C);
	struct func_c *func = (struct func_c *) base;
	func_c_unload(func);
	TRASH(base);
	free(func);
}

/**
 * Resolve func->func (find the respective DLL and fetch the
 * symbol from it).
 */
static int
func_c_load(struct func_c *func)
{
	assert(func->func == NULL);

	struct func_name name;
	func_split_name(func->base.def->name, &name);

	struct module *cached, *module;
	struct cmod *cmod;

	cached = module_cache_find(name.package, name.package_end);
	if (cached != NULL) {
		module = cached;
		goto resolve_sym;
	}

	size_t len = name.package_end - name.package;
	cmod = cmod_cache_find(name.package, len);
	if (cmod == NULL) {
		/*
		 * The module is not present in both
		 * box.schema.func cache and in cmod
		 * cache. Thus load it from from the
		 * scratch and put into cmod cache
		 * as well.
		 */
		module = module_load(name.package, name.package_end);
		if (module == NULL)
			return -1;
		if (cmod_cache_put(module->cmod) != 0) {
			module_delete(module);
			return -1;
		}

		/*
		 * Fresh cmod instance is bound to
		 * the module and get unref upon
		 * module unload.
		 */
		cmod = NULL;
	} else {
		/*
		 * Someone already has loaded this
		 * shared library via cmod interface,
		 * thus simply increase the reference
		 * (and don't forget to unref later).
		 */
		module = module_new(cmod);
		if (module == NULL)
			return -1;
		cmod_ref(cmod);
	}

	if (module_cache_put(module)) {
		if (cmod != NULL)
			cmod_unref(cmod);
		module_delete(module);
		return -1;
	}

resolve_sym:
	func->func = module_sym(module, name.sym);
	if (func->func == NULL) {
		if (cached == NULL) {
			/*
			 * In case if it was a first load we should
			 * clean the cache immediately otherwise
			 * the module continue being referenced even
			 * if there will be no use of it.
			 *
			 * Note the module_sym set an error thus be
			 * careful to not wipe it.
			 */
			module_cache_del(name.package, name.package_end);
			module_delete(module);
		}
		return -1;
	}
	func->module = module;
	rlist_add(&module->funcs, &func->item);
	return 0;
}

int
func_c_call(struct func *base, struct port *args, struct port *ret)
{
	assert(base->vtab == &func_c_vtab);
	assert(base != NULL && base->def->language == FUNC_LANGUAGE_C);
	struct func_c *func = (struct func_c *) base;
	if (func->func == NULL) {
		if (func_c_load(func) != 0)
			return -1;
	}

	struct module *module = func->module;
	int rc = cmod_call(module->cmod, func->func, args, ret);
	module_gc(module);
	return rc;
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
