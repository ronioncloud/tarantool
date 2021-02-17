/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2021, Tarantool AUTHORS, please see AUTHORS file.
 */

#include <unistd.h>
#include <string.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <lua.h>

#include "box/error.h"
#include "box/port.h"

#include "tt_static.h"

#include "assoc.h"
#include "cmod.h"
#include "fiber.h"

#include "lua/utils.h"
#include "libeio/eio.h"

/**
 * Function descriptor.
 */
struct cmod_func {
	/** Module function belongs to. */
	struct cmod *cmod;
	/** Address to execute on call. */
	box_function_f addr;
	/** Number of references. */
	int64_t refs;
	/** Length of functon name in @a key. */
	size_t sym_len;
	/** Length of @a key. */
	size_t len;
	/** Function hash key. */
	char key[0];
};

/** Module name to cmod hash. */
static struct mh_strnptr_t *cmod_hash = NULL;

/** Function name to cmod_func hash. */
static struct mh_strnptr_t *cmod_func_hash = NULL;

/** A type to find a module from an object. */
static const char *uname_cmod = "tt_uname_cmod";

/** A type to find a function from an object. */
static const char *uname_func = "tt_uname_cmod_func";

/** Module unique IDs. */
static int64_t cmod_ids = 1;

/** Get data associated with an object. */
static void *
get_udata(struct lua_State *L, const char *uname)
{
	void **pptr = luaL_testudata(L, 1, uname);
	return pptr != NULL ? *pptr : NULL;
}

/** Set data to a new value. */
static void
set_udata(struct lua_State *L, const char *uname, void *ptr)
{
	void **pptr = luaL_testudata(L, 1, uname);
	assert(pptr != NULL);
	*pptr = ptr;
}

/** Setup a new data and associate it with an object. */
static void
new_udata(struct lua_State *L, const char *uname, void *ptr)
{
	*(void **)lua_newuserdata(L, sizeof(void *)) = ptr;
	luaL_getmetatable(L, uname);
	lua_setmetatable(L, -2);
}

/**
 * Helpers for string based hash manipulations.
 */
static void *
hash_find(struct mh_strnptr_t *h, const char *str, size_t len)
{
	mh_int_t e = mh_strnptr_find_inp(h, str, len);
	if (e == mh_end(h))
		return NULL;
	return mh_strnptr_node(h, e)->val;
}

static void
hash_update(struct mh_strnptr_t *h, const char *src_str, size_t len,
	    const char *new_str, void *new_val)
{
	mh_int_t e = mh_strnptr_find_inp(h, src_str, len);
	if (e == mh_end(h))
		panic("cmod: failed to update hash");
	mh_strnptr_node(h, e)->str = new_str;
	mh_strnptr_node(h, e)->val = new_val;
}

static int
hash_add(struct mh_strnptr_t *h, const char *str, size_t len, void *val)
{
	const struct mh_strnptr_node_t nd = {
		.str	= str,
		.len	= len,
		.hash	= mh_strn_hash(str, len),
		.val	= val,
	};
	if (mh_strnptr_put(h, &nd, NULL, NULL) == mh_end(h)) {
		diag_set(OutOfMemory, sizeof(nd), "malloc",
			 "cmod: hash node");
		return -1;
	}
	return 0;
}

static void
hash_del_kv(struct mh_strnptr_t *h, const char *str,
	    size_t len, void *val)
{
	mh_int_t e = mh_strnptr_find_inp(h, str, len);
	if (e != mh_end(h)) {
		void *v = mh_strnptr_node(h, e)->val;
		if (v == val)
			mh_strnptr_del(h, e, NULL);
	}
}

static void
hash_del(struct mh_strnptr_t *h, const char *str, size_t len)
{
	mh_int_t e = mh_strnptr_find_inp(h, str, len);
	if (e != mh_end(h))
		mh_strnptr_del(h, e, NULL);
}

/** Arguments for lpackage_search. */
struct find_ctx {
	const char *package;
	size_t package_len;
	char *path;
	size_t path_len;
};

/** A cpcall() helper for lfind_package(). */
static int
lpackage_search(lua_State *L)
{
	struct find_ctx *ctx = (void *)lua_topointer(L, 1);

	lua_getglobal(L, "package");
	lua_getfield(L, -1, "search");
	lua_pushlstring(L, ctx->package, ctx->package_len);

	lua_call(L, 1, 1);
	if (lua_isnil(L, -1))
		return luaL_error(L, "cmod: module not found");

	char resolved[PATH_MAX];
	if (realpath(lua_tostring(L, -1), resolved) == NULL) {
		diag_set(SystemError, "cmod: realpath");
		return luaT_error(L);
	}

	/*
	 * No need for result being trimmed test, it
	 * is guaranteed by realpath call.
	 */
	snprintf(ctx->path, ctx->path_len, "%s", resolved);
	return 0;
}

int
cmod_find_package(const char *package, size_t package_len,
		  char *path, size_t path_len)
{
	struct find_ctx ctx = {
		.package	= package,
		.package_len	= package_len,
		.path		= path,
		.path_len	= path_len,
	};

	lua_State *L = tarantool_L;
	int top = lua_gettop(L);
	if (luaT_cpcall(L, lpackage_search, &ctx) != 0) {
		diag_set(ClientError, ER_LOAD_MODULE, ctx.package_len,
			 ctx.package, lua_tostring(L, -1));
		lua_settop(L, top);
		return -1;
	}
	assert(top == lua_gettop(L));
	return 0;
}

/** Increase reference to cmod. */
void
cmod_ref(struct cmod *m)
{
	assert(m->refs >= 0);
	++m->refs;
}

/** Decrease reference to cmod and free it if last one. */
void
cmod_unref(struct cmod *m)
{
	assert(m->refs > 0);
	if (--m->refs == 0) {
		hash_del_kv(cmod_hash, m->package, m->package_len, m);
		dlclose(m->handle);
		TRASH(m);
		free(m);
	}
}

struct cmod *
cmod_cache_find(const char *package, size_t package_len)
{
	return hash_find(cmod_hash, package, package_len);
}

int
cmod_cache_put(struct cmod *m)
{
	return hash_add(cmod_hash, m->package, m->package_len, m);
}

/** Fill cmod attributes from stat. */
static void
cmod_attr_fill(struct cmod_attr *attr, struct stat *st)
{
	attr->st_dev = st->st_dev;
	attr->st_ino = st->st_ino;
	attr->st_size = st->st_size;
#ifdef TARGET_OS_DARWIN
	attr->st_mtimespec = st->st_mtimespec;
#else
	attr->st_mtim = st->st_mtim;
#endif
}

struct cmod *
cmod_new(const char *package, size_t package_len, const char *source_path)
{
	size_t size = sizeof(struct cmod) + package_len + 1;
	struct cmod *m = malloc(size);
	if (m == NULL) {
		diag_set(OutOfMemory, size, "malloc", "cmod");
		return NULL;
	}

	m->package_len = package_len;
	m->refs = 0;

	memcpy(m->package, package, package_len);
	m->package[package_len] = 0;

	const char *tmpdir = getenv("TMPDIR");
	if (tmpdir == NULL)
		tmpdir = "/tmp";

	char dir_name[PATH_MAX];
	int rc = snprintf(dir_name, sizeof(dir_name),
			  "%s/tntXXXXXX", tmpdir);
	if (rc < 0 || (size_t)rc >= sizeof(dir_name)) {
		diag_set(SystemError, "failed to generate path to tmp dir");
		goto error;
	}

	if (mkdtemp(dir_name) == NULL) {
		diag_set(SystemError, "failed to create unique dir name: %s",
			 dir_name);
		goto error;
	}

	char load_name[PATH_MAX];
	rc = snprintf(load_name, sizeof(load_name),
		      "%s/%.*s." TARANTOOL_LIBEXT,
		      dir_name, (int)package_len, package);
	if (rc < 0 || (size_t)rc >= sizeof(dir_name)) {
		diag_set(SystemError, "failed to generate path to DSO");
		goto error;
	}

	struct stat st;
	if (stat(source_path, &st) < 0) {
		diag_set(SystemError, "failed to stat() module: %s",
			 source_path);
		goto error;
	}
	cmod_attr_fill(&m->attr, &st);

	int source_fd = open(source_path, O_RDONLY);
	if (source_fd < 0) {
		diag_set(SystemError, "failed to open module %s "
			 "file for reading", source_path);
		goto error;
	}
	int dest_fd = open(load_name, O_WRONLY | O_CREAT | O_TRUNC,
			   st.st_mode & 0777);
	if (dest_fd < 0) {
		diag_set(SystemError, "failed to open file %s "
			 "for writing ", load_name);
		close(source_fd);
		goto error;
	}

	off_t ret = eio_sendfile_sync(dest_fd, source_fd, 0, st.st_size);
	close(source_fd);
	close(dest_fd);
	if (ret != st.st_size) {
		diag_set(SystemError, "failed to copy DSO %s to %s",
			 source_path, load_name);
		goto error;
	}

	m->handle = dlopen(load_name, RTLD_NOW | RTLD_LOCAL);
	if (unlink(load_name) != 0)
		say_warn("failed to unlink dso link: %s", load_name);
	if (rmdir(dir_name) != 0)
		say_warn("failed to delete temporary dir: %s", dir_name);
	if (m->handle == NULL) {
		diag_set(ClientError, ER_LOAD_MODULE, package_len,
			  package, dlerror());
		goto error;
	}

	m->id = cmod_ids++;
	cmod_ref(m);
	return m;

error:
	free(m);
	return NULL;
}

/**
 * Load a module.
 *
 * This function takes a module path from the caller
 * stack @a L and returns cached module instance or
 * creates a new module object.
 *
 * Possible errors:
 *
 * - IllegalParams: module path is either not supplied
 *   or not a string.
 * - SystemError: unable to open a module due to a system error.
 * - ClientError: a module does not exist.
 * - OutOfMemory: unable to allocate a module.
 *
 * @returns module object on success or throws an error.
 */
static int
lcmod_load(struct lua_State *L)
{
	const char msg_noname[] = "Expects cmod.load(\'name\') "
		"but no name passed";

	if (lua_gettop(L) != 1 || !lua_isstring(L, 1)) {
		diag_set(IllegalParams, msg_noname);
		return luaT_error(L);
	}

	size_t name_len;
	const char *name = lua_tolstring(L, 1, &name_len);

	if (name_len < 1) {
		diag_set(IllegalParams, msg_noname);
		return luaT_error(L);
	}

	char path[PATH_MAX];
	if (cmod_find_package(name, name_len, path, sizeof(path)) != 0)
		return luaT_error(L);

	struct cmod *m = hash_find(cmod_hash, name, name_len);
	if (m != NULL) {
		struct cmod_attr attr;
		struct stat st;
		if (stat(path, &st) != 0) {
			diag_set(SystemError, "failed to stat() %s",
				 path);
			return luaT_error(L);
		}

		/*
		 * In case of cache hit we may reuse existing
		 * module which speedup load procedure.
		 */
		cmod_attr_fill(&attr, &st);
		if (memcmp(&attr, &m->attr, sizeof(attr)) == 0) {
			cmod_ref(m);
			new_udata(L, uname_cmod, m);
			return 1;
		}

		/*
		 * Module has been updated on a storage device,
		 * so load a new instance and update the cache,
		 * old entry get evicted but continue residing
		 * in memory, fully functional, until last
		 * function is unloaded.
		 */
		m = cmod_new(name, name_len, path);
		if (m == NULL)
			return luaT_error(L);

		hash_update(cmod_hash, name, name_len, m->package, m);
		/*
		 * This is transparent procedure so notify a user
		 * that new module is read otherwise it won't be
		 * possible to figure out what is going on.
		 */
		say_info("cmod: attr change, reload: %s", name);
	} else {
		m = cmod_new(name, name_len, path);
		if (m == NULL)
			return luaT_error(L);
		if (hash_add(cmod_hash, m->package, name_len, m) != 0) {
			/* Never been in hash: safe for hash_del_kv */
			cmod_unref(m);
			return luaT_error(L);
		}
	}

	new_udata(L, uname_cmod, m);
	return 1;
}

/**
 * Unload a module.
 *
 * Take a module object from the caller stack @a L and unload it.
 *
 * Possible errors:
 *
 * - IllegalParams: module is not supplied.
 * - IllegalParams: the module is unloaded.
 *
 * @returns true on success or throwns an error.
 */
static int
lcmod_unload(struct lua_State *L)
{
	if (lua_gettop(L) != 1) {
		diag_set(IllegalParams, "Expects module:unload()");
		return luaT_error(L);
	}

	struct cmod *m = get_udata(L, uname_cmod);
	if (m == NULL) {
		diag_set(IllegalParams, "The module is unloaded");
		return luaT_error(L);
	}

	set_udata(L, uname_cmod, NULL);
	cmod_unref(m);
	lua_pushboolean(L, true);
	return 1;
}

/** Handle __index request for a module object. */
static int
lcmod_index(struct lua_State *L)
{
	lua_getmetatable(L, 1);
	lua_pushvalue(L, 2);
	lua_rawget(L, -2);
	if (!lua_isnil(L, -1))
		return 1;

	struct cmod *m = get_udata(L, uname_cmod);
	if (m == NULL) {
		lua_pushnil(L);
		return 1;
	}

	const char *key = lua_tostring(L, 2);
	if (key == NULL || lua_type(L, 2) != LUA_TSTRING) {
		diag_set(IllegalParams,
			 "Bad params, use __index(obj, <string>)");
		return luaT_error(L);
	}

	if (strcmp(key, "path") == 0) {
		lua_pushstring(L, m->package);
		return 1;
	}

	/*
	 * Internal keys for debug only, not API.
	 */
	if (strncmp(key, "tt_dev.", 7) == 0) {
		const char *subkey = &key[7];
		if (strcmp(subkey, "refs") == 0) {
			lua_pushnumber(L, m->refs);
			return 1;
		} else if (strcmp(subkey, "id") == 0) {
			lua_pushnumber(L, m->id);
			return 1;
		}
	}
	return 0;
}

/** Module representation for REPL (console). */
static int
lcmod_serialize(struct lua_State *L)
{
	struct cmod *m = get_udata(L, uname_cmod);
	if (m == NULL) {
		lua_pushnil(L);
		return 1;
	}

	lua_createtable(L, 0, 1);
	lua_pushstring(L, m->package);
	lua_setfield(L, -2, "path");
	return 1;
}

/** Collect a module. */
static int
lcmod_gc(struct lua_State *L)
{
	struct cmod *m = get_udata(L, uname_cmod);
	if (m != NULL) {
		set_udata(L, uname_cmod, NULL);
		cmod_unref(m);
	}
	return 0;
}

/** Increase reference to a function. */
static void
cmod_func_ref(struct cmod_func *cf)
{
	assert(cf->refs >= 0);
	++cf->refs;
}

/** Free function memory. */
static void
cmod_func_delete(struct cmod_func *cf)
{
	TRASH(cf);
	free(cf);
}

/** Unreference a function and free if last one. */
static void
cmod_func_unref(struct cmod_func *cf)
{
	assert(cf->refs > 0);
	if (--cf->refs == 0) {
		cmod_unref(cf->cmod);
		hash_del(cmod_func_hash, cf->key, cf->len);
		cmod_func_delete(cf);
	}
}

/** Function name from a hash key. */
static char *
cmod_func_name(struct cmod_func *cf)
{
	return &cf->key[cf->len - cf->sym_len];
}

/**
 * Allocate a new function instance and resolve its address.
 *
 * @param m a module the function should be loaded from.
 * @param key function hash key, ie "1.module.foo".
 * @param len length of @a key.
 * @param sym_len function symbol name length, ie 3 for "foo".
 *
 * @returns function instance on success, NULL otherwise (diag is set).
 */
static struct cmod_func *
cmod_func_new(struct cmod *m, const char *key, size_t len, size_t sym_len)
{
	size_t size = sizeof(struct cmod_func) + len + 1;
	struct cmod_func *cf = malloc(size);
	if (cf == NULL) {
		diag_set(OutOfMemory, size, "malloc", "cf");
		return NULL;
	}

	cf->cmod = m;
	cf->len = len;
	cf->sym_len = sym_len;
	cf->refs = 0;

	memcpy(cf->key, key, len);
	cf->key[len] = '\0';

	cf->addr = dlsym(m->handle, cmod_func_name(cf));
	if (cf->addr == NULL) {
		diag_set(ClientError, ER_LOAD_FUNCTION,
			 cmod_func_name(cf), dlerror());
		cmod_func_delete(cf);
		return NULL;
	}

	if (hash_add(cmod_func_hash, cf->key, cf->len, cf) != 0) {
		cmod_func_delete(cf);
		return NULL;
	}

	/*
	 * Each new function depends on module presence.
	 * Module will reside even if been unload
	 * explicitly after the function creation.
	 */
	cmod_ref(cf->cmod);
	cmod_func_ref(cf);
	return cf;
}

/**
 * Load a function.
 *
 * This function takes a function name from the caller
 * stack @a L and either returns a cached function or
 * creates a new function object.
 *
 * Possible errors:
 *
 * - IllegalParams: function name is either not supplied
 *   or not a string.
 * - SystemError: unable to open a module due to a system error.
 * - ClientError: a module does not exist.
 * - OutOfMemory: unable to allocate a module.
 *
 * @returns module object on success or throws an error.
 */
static int
lcmod_load_func(struct lua_State *L)
{
	const char *method = "function = module:load";
	const char fmt_noname[] = "Expects %s(\'name\') but no name passed";

	if (lua_gettop(L) != 2 || !lua_isstring(L, 2)) {
		diag_set(IllegalParams, fmt_noname, method);
		return luaT_error(L);
	}

	struct cmod *m = get_udata(L, uname_cmod);
	if (m == NULL) {
		const char *fmt =
			"Expects %s(\'name\') but not module object passed";
		diag_set(IllegalParams, fmt, method);
		return luaT_error(L);
	}

	size_t sym_len;
	const char *sym = lua_tolstring(L, 2, &sym_len);

	if (sym_len < 1) {
		diag_set(IllegalParams, fmt_noname, method);
		return luaT_error(L);
	}

	/*
	 * Functions are bound to a module symbols, thus
	 * since the hash is global it should be unique
	 * per module. The symbol (function name) is the
	 * last part of the hash key.
	 */
	const char *key = tt_sprintf("%lld.%s.%s", (long long)m->id,
				     m->package, sym);
	size_t len = strlen(key);

	struct cmod_func *cf = hash_find(cmod_func_hash, key, len);
	if (cf == NULL) {
		cf = cmod_func_new(m, key, len, sym_len);
		if (cf == NULL)
			return luaT_error(L);
	} else {
		cmod_func_ref(cf);
	}

	new_udata(L, uname_func, cf);
	return 1;
}

/**
 * Unload a function.
 *
 * Take a function object from the caller stack @a L and unload it.
 *
 * Possible errors:
 *
 * - IllegalParams: the function is not supplied.
 * - IllegalParams: the function already unloaded.
 *
 * @returns true on success or throwns an error.
 */
static int
lfunc_unload(struct lua_State *L)
{
	if (lua_gettop(L) != 1) {
		diag_set(IllegalParams, "Expects function:unload()");
		return luaT_error(L);
	}

	struct cmod_func *cf = get_udata(L, uname_func);
	if (cf == NULL) {
		diag_set(IllegalParams, "The function is unloaded");
		return luaT_error(L);
	}

	set_udata(L, uname_func, NULL);
	cmod_func_unref(cf);

	lua_pushboolean(L, true);
	return 1;
}

/** Handle __index request for a function object. */
static int
lfunc_index(struct lua_State *L)
{
	lua_getmetatable(L, 1);
	lua_pushvalue(L, 2);
	lua_rawget(L, -2);
	if (!lua_isnil(L, -1))
		return 1;

	struct cmod_func *cf = get_udata(L, uname_func);
	if (cf == NULL) {
		lua_pushnil(L);
		return 1;
	}

	const char *key = lua_tostring(L, 2);
	if (key == NULL || lua_type(L, 2) != LUA_TSTRING) {
		diag_set(IllegalParams,
			 "Bad params, use __index(obj, <string>)");
		return luaT_error(L);
	}

	if (strcmp(key, "name") == 0) {
		lua_pushstring(L, cmod_func_name(cf));
		return 1;
	}

	/*
	 * Internal keys for debug only, not API.
	 */
	if (strncmp(key, "tt_dev.", 7) == 0) {
		const char *subkey = &key[7];
		if (strcmp(subkey, "refs") == 0) {
			lua_pushnumber(L, cf->refs);
			return 1;
		} else if (strcmp(subkey, "key") == 0) {
			lua_pushstring(L, cf->key);
			return 1;
		} else if (strcmp(subkey, "cmod.id") == 0) {
			lua_pushnumber(L, cf->cmod->id);
			return 1;
		} else if (strcmp(subkey, "cmod.refs") == 0) {
			lua_pushnumber(L, cf->cmod->refs);
			return 1;
		}
	}
	return 0;
}

/** Function representation for REPL (console). */
static int
lfunc_serialize(struct lua_State *L)
{
	struct cmod_func *cf = get_udata(L, uname_func);
	if (cf == NULL) {
		lua_pushnil(L);
		return 1;
	}

	lua_createtable(L, 0, 1);
	lua_pushstring(L, cmod_func_name(cf));
	lua_setfield(L, -2, "name");
	return 1;
}

/** Collect a function. */
static int
lfunc_gc(struct lua_State *L)
{
	struct cmod_func *cf = get_udata(L, uname_func);
	if (cf != NULL) {
		set_udata(L, uname_func, NULL);
		cmod_func_unref(cf);
	}
	return 0;
}

int
cmod_call(struct cmod *m, box_function_f func_addr,
	  struct port *args, struct port *ret)
{
	struct region *region = &fiber()->gc;
	size_t region_svp = region_used(region);

	uint32_t data_sz;
	const char *data = port_get_msgpack(args, &data_sz);
	if (data == NULL)
		return -1;

	port_c_create(ret);
	box_function_ctx_t ctx = {
		.port = ret,
	};

	/*
	 * The function may get rescheduled inside,
	 * thus make sure the module won't disappear.
	 */
	cmod_ref(m);
	int rc = func_addr(&ctx, data, data + data_sz);
	cmod_unref(m);
	region_truncate(region, region_svp);

	if (rc != 0) {
		if (diag_last_error(&fiber()->diag) == NULL)
			diag_set(ClientError, ER_PROC_C, "unknown error");
		port_destroy(ret);
		return -1;
	}

	return 0;
}

/** Call a function by its name from the Lua code. */
static int
lfunc_call(struct lua_State *L)
{
	struct cmod_func *cf = get_udata(L, uname_func);
	if (cf == NULL) {
		diag_set(IllegalParams, "The function is unloaded");
		return luaT_error(L);
	}

	lua_State *args_L = luaT_newthread(tarantool_L);
	if (args_L == NULL)
		return luaT_error(L);

	int coro_ref = luaL_ref(tarantool_L, LUA_REGISTRYINDEX);
	lua_xmove(L, args_L, lua_gettop(L) - 1);

	struct port args;
	port_lua_create(&args, args_L);
	((struct port_lua *)&args)->ref = coro_ref;

	struct port ret;

	if (cmod_call(cf->cmod, cf->addr, &args, &ret) != 0) {
		port_destroy(&args);
		return luaT_error(L);
	}

	int top = lua_gettop(L);
	port_dump_lua(&ret, L, true);
	int cnt = lua_gettop(L) - top;

	port_destroy(&ret);
	port_destroy(&args);

	return cnt;
}

/** Initialize cmod. */
void
box_lua_cmod_init(struct lua_State *L)
{
	cmod_func_hash = mh_strnptr_new();
	if (cmod_func_hash == NULL)
		panic("cmod: Can't allocate func hash table");
	cmod_hash = mh_strnptr_new();
	if (cmod_hash == NULL)
		panic("cmod: Can't allocate cmod hash table");

	(void)L;
	static const struct luaL_Reg top_methods[] = {
		{ "load",		lcmod_load		},
		{ NULL, NULL },
	};
	luaL_register_module(L, "cmod", top_methods);
	lua_pop(L, 1);

	static const struct luaL_Reg lcmod_methods[] = {
		{ "unload",		lcmod_unload		},
		{ "load",		lcmod_load_func		},
		{ "__index",		lcmod_index		},
		{ "__serialize",	lcmod_serialize		},
		{ "__gc",		lcmod_gc		},
		{ NULL, NULL },
	};
	luaL_register_type(L, uname_cmod, lcmod_methods);

	static const struct luaL_Reg lfunc_methods[] = {
		{ "unload",		lfunc_unload		},
		{ "__index",		lfunc_index		},
		{ "__serialize",	lfunc_serialize		},
		{ "__gc",		lfunc_gc		},
		{ "__call",		lfunc_call		},
		{ NULL, NULL },
	};
	luaL_register_type(L, uname_func, lfunc_methods);
}
