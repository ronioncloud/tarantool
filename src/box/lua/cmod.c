/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2021, Tarantool AUTHORS, please see AUTHORS file.
 */

#include <string.h>
#include <lua.h>

#include "assoc.h"
#include "diag.h"

#include "box/module_cache.h"
#include "box/error.h"
#include "box/port.h"
#include "tt_static.h"

#include "trivia/util.h"
#include "lua/utils.h"

/**
 * Function descriptor.
 */
struct cmod_func {
	/**
	 * Symbol descriptor for the function in
	 * an associated module.
	 */
	struct module_sym mod_sym;
	/**
	 * Length of @a name member.
	 */
	size_t len;
	/**
	 * Count of active references to the function.
	 */
	int64_t refs;
	/**
	 * Module path with function name separated
	 * by a point, like "module.func".
	 */
	char name[0];
};

/** A type to find a module from an object. */
static const char *cmod_module_uname = "cmod_module_uname";

/** A type to find a function from an object. */
static const char *cmod_func_uname = "cmod_func_uname";

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
 * Function name to cmod_func hash. The name includes
 * module package path without file extension.
 */
static struct mh_strnptr_t *func_hash = NULL;

/**
 * Find function in cmod_func hash.
 */
struct cmod_func *
func_cache_find(const char *name, size_t name_len)
{
	mh_int_t e = mh_strnptr_find_inp(func_hash, name, name_len);
	if (e == mh_end(func_hash))
		return NULL;
	return mh_strnptr_node(func_hash, e)->val;
}

/**
 * Delete a function instance from cmod_func hash.
 */
static void
func_cache_del(struct cmod_func *cf)
{
	assert(cf->refs == 0);

	mh_int_t e = mh_strnptr_find_inp(func_hash, cf->name, cf->len);
	assert(e != mh_end(func_hash));
	mh_strnptr_del(func_hash, e, NULL);
}

/**
 * Add a function instance into cmod_func hash.
 */
static int
func_cache_add(struct cmod_func *cf)
{
	const struct mh_strnptr_node_t nd = {
		.str	= cf->name,
		.len	= cf->len,
		.hash	= mh_strn_hash(cf->name, cf->len),
		.val	= cf,
	};

	mh_int_t e = mh_strnptr_put(func_hash, &nd, NULL, NULL);
	if (e == mh_end(func_hash)) {
		diag_set(OutOfMemory, sizeof(nd),
			 "malloc", "cmod_func node");
		return -1;
	}
	return 0;
}

/**
 * Unload a symbol and free a function instance.
 */
static void
func_delete(struct cmod_func *cf)
{
	assert(cf->refs == 0);
	module_sym_unload(&cf->mod_sym);
	TRASH(cf);
	free(cf);
}

/**
 * Increase reference to a function.
 */
static void
func_ref(struct cmod_func *cf)
{
	assert(cf->refs >= 0);
	cf->refs++;
}

/**
 * Decrease a function reference and delete it if last one.
 */
static void
func_unref(struct cmod_func *cf)
{
	assert(cf->refs > 0);
	if (cf->refs-- == 1) {
		func_cache_del(cf);
		func_delete(cf);
	}
}

/**
 * Allocate a new function instance and resolve a symbol address.
 *
 * @param module module the function load from.
 * @param name package path and a function name, ie "module.foo".
 * @param len length of @a name.
 *
 * @returns function instance on success, NULL otherwise setting diag area.
 */
static struct cmod_func *
func_new(struct module *m, const char *name, size_t len)
{
	size_t size = sizeof(struct cmod_func) + len + 1;
	struct cmod_func *cf = malloc(size);
	if (cf == NULL) {
		diag_set(OutOfMemory, size, "malloc", "cf");
		return NULL;
	}

	cf->mod_sym.addr	= NULL;
	cf->mod_sym.module	= m;
	cf->mod_sym.name	= cf->name;
	cf->len			= len;
	cf->refs		= 0;

	memcpy(cf->name, name, len);
	cf->name[len] = '\0';

	if (module_sym_load(&cf->mod_sym, false) != 0) {
		func_delete(cf);
		return NULL;
	}

	func_ref(cf);
	return cf;
}

/**
 * Load a new function.
 *
 * This function takes a function name from the caller
 * stack @a L and creates a new function object. If
 * the function is already loaded we simply return
 * a reference to existing one.
 *
 * Possible errors:
 *
 * - IllegalParams: function name is either not supplied
 *   or not a string.
 * - IllegalParams: function references limit exceeded.
 * - OutOfMemory: unable to allocate a function.
 * - ClientError: no such function in the module.
 * - ClientError: module has been updated on disk and not
 *   yet unloaded and loaded back.
 *
 * @returns function object on success or throwns an error.
 */
static int
lcmod_func_load(struct lua_State *L)
{
	const char *method = "function = module:load";

	if (lua_gettop(L) != 2 || !lua_isstring(L, 2)) {
		const char *fmt =
			"Expects %s(\'name\') but no name passed";
		diag_set(IllegalParams, fmt, method);
		return luaT_error(L);
	}

	struct module *m = get_udata(L, cmod_module_uname);
	if (m == NULL) {
		const char *fmt =
			"Expects %s(\'name\') but not module object passed";
		diag_set(IllegalParams, fmt, method);
		return luaT_error(L);
	}

	const char *func_name = lua_tostring(L, 2);
	const char *name = tt_sprintf("%s.%s", m->package, func_name);
	size_t len = strlen(name);

	/*
	 * We try to reuse already allocated function in
	 * case if someone is loading same function twise.
	 * This will save memory and eliminates redundant
	 * symbol address resolving.
	 */
	struct cmod_func *cf = func_cache_find(name, len);
	if (cf == NULL) {
		cf = func_new(m, name, len);
		if (cf == NULL)
			return luaT_error(L);
		if (func_cache_add(cf) != 0) {
			func_unref(cf);
			return luaT_error(L);
		}
	} else {
		func_ref(cf);
	}

	new_udata(L, cmod_func_uname, cf);
	return 1;
}

/**
 * Unload a function.
 *
 * This function takes a function object from
 * the caller stack @a L and unloads it.
 *
 * Possible errors:
 *
 * - IllegalParams: function is not supplied.
 * - IllegalParams: the function does not exist.
 *
 * @returns true on success or throwns an error.
 */
static int
lcmod_func_unload(struct lua_State *L)
{
	if (lua_gettop(L) != 1) {
		diag_set(IllegalParams, "Expects function:unload()");
		return luaT_error(L);
	}

	struct cmod_func *cf = get_udata(L, cmod_func_uname);
	if (cf == NULL) {
		diag_set(IllegalParams, "The function is unloaded");
		return luaT_error(L);
	}

	set_udata(L, cmod_func_uname, NULL);
	func_unref(cf);

	lua_pushboolean(L, true);
	return 1;
}

/**
 * Load a new module.
 *
 * This function takes a module patch from the caller
 * stack @a L and creates a new module object.
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
lcmod_module_load(struct lua_State *L)
{
	if (lua_gettop(L) != 1 || !lua_isstring(L, 1)) {
		diag_set(IllegalParams, "Expects cmod.load(\'name\') "
			 "but no name passed");
		return luaT_error(L);
	}

	size_t name_len;
	const char *name = lua_tolstring(L, 1, &name_len);

	struct module *module = module_load(name, &name[name_len]);
	if (module == NULL)
		return luaT_error(L);

	new_udata(L, cmod_module_uname, module);
	return 1;
}

/**
 * Unload a module.
 *
 * This function takes a module object from
 * the caller stack @a L and unloads it.
 *
 * If there are some active functions left then
 * module won't be freed internally until last function
 * from this module is unloaded, this is guaranteed by
 * module_cache engine.
 *
 * Possible errors:
 *
 * - IllegalParams: module is not supplied.
 * - IllegalParams: module already unloaded.
 *
 * @returns true on success or throws an error.
 */
static int
lcmod_module_unload(struct lua_State *L)
{
	if (lua_gettop(L) != 1) {
		diag_set(IllegalParams, "Expects module:unload()");
		return luaT_error(L);
	}

	struct module *m = get_udata(L, cmod_module_uname);
	if (m == NULL) {
		diag_set(IllegalParams, "The module is already unloaded");
		return luaT_error(L);
	}
	set_udata(L, cmod_module_uname, NULL);
	module_unload(m);
	lua_pushboolean(L, true);
	return 1;
}

static const char *
module_state_str(struct module *m)
{
	return module_is_orphan(m) ? "orphan" : "cached";
}

/**
 * Handle __index request for a module object.
 */
static int
lcmod_module_index(struct lua_State *L)
{
	/*
	 * Instead of showing userdata pointer
	 * lets provide a serialized value.
	 */
	lua_getmetatable(L, 1);
	lua_pushvalue(L, 2);
	lua_rawget(L, -2);
	if (!lua_isnil(L, -1))
		return 1;

	struct module *m = get_udata(L, cmod_module_uname);
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
	} else if (strcmp(key, "state") == 0) {
		lua_pushstring(L, module_state_str(m));
		return 1;
	}

	return 0;
}

/**
 * Module handle representation for REPL (console).
 */
static int
lcmod_module_serialize(struct lua_State *L)
{
	struct module *m = get_udata(L, cmod_module_uname);
	if (m == NULL) {
		lua_pushnil(L);
		return 1;
	}

	lua_createtable(L, 0, 2);
	lua_pushstring(L, m->package);
	lua_setfield(L, -2, "path");
	lua_pushstring(L, module_state_str(m));
	lua_setfield(L, -2, "state");

	return 1;
}

/**
 * Collect a module handle.
 */
static int
lcmod_module_gc(struct lua_State *L)
{
	struct module *m = get_udata(L, cmod_module_uname);
	if (m != NULL) {
		set_udata(L, cmod_module_uname, NULL);
		module_unload(m);
	}
	return 0;
}

/**
 * Function handle representation for REPL (console).
 */
static int
lcmod_func_serialize(struct lua_State *L)
{
	struct cmod_func *cf = get_udata(L, cmod_func_uname);
	if (cf == NULL) {
		lua_pushnil(L);
		return 1;
	}

	lua_createtable(L, 0, 1);
	lua_pushstring(L, cf->name);
	lua_setfield(L, -2, "name");

	return 1;
}

/**
 * Handle __index request for a function object.
 */
static int
lcmod_func_index(struct lua_State *L)
{
	/*
	 * Instead of showing userdata pointer
	 * lets provide a serialized value.
	 */
	lua_getmetatable(L, 1);
	lua_pushvalue(L, 2);
	lua_rawget(L, -2);
	if (!lua_isnil(L, -1))
		return 1;

	struct cmod_func *cf = get_udata(L, cmod_func_uname);
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
		lua_pushstring(L, cf->name);
		return 1;
	}

	return 0;
}

/**
 * Collect function handle if there is no active loads left.
 */
static int
lcmod_func_gc(struct lua_State *L)
{
	struct cmod_func *cf = get_udata(L, cmod_func_uname);
	if (cf != NULL) {
		set_udata(L, cmod_func_uname, NULL);
		func_unref(cf);
	}
	return 0;
}

/**
 * Call a function by its name from the Lua code.
 */
static int
lcmod_func_call(struct lua_State *L)
{
	struct cmod_func *cf = get_udata(L, cmod_func_uname);
	if (cf == NULL) {
		diag_set(IllegalParams, "The function is unloaded");
		return luaT_error(L);
	}

	/*
	 * FIXME: We should get rid of luaT_newthread but this
	 * requires serious modifications. In particular
	 * port_lua_do_dump uses tarantool_L reference and
	 * coro_ref must be valid as well.
	 */
	lua_State *args_L = luaT_newthread(tarantool_L);
	if (args_L == NULL)
		return luaT_error(L);

	int coro_ref = luaL_ref(tarantool_L, LUA_REGISTRYINDEX);
	lua_xmove(L, args_L, lua_gettop(L) - 1);

	struct port args;
	port_lua_create(&args, args_L);
	((struct port_lua *)&args)->ref = coro_ref;

	struct port ret;
	if (module_sym_call(&cf->mod_sym, &args, &ret) != 0) {
		port_destroy(&args);
		return luaT_error(L);
	}

	int top = lua_gettop(L);
	lua_pushboolean(L, true);
	port_dump_lua(&ret, L, true);
	int cnt = lua_gettop(L) - top;

	port_destroy(&ret);
	port_destroy(&args);

	return cnt;
}

/**
 * Initialize cmod module.
 */
void
box_lua_cmod_init(struct lua_State *L)
{
	func_hash = mh_strnptr_new();
	if (func_hash == NULL) {
		panic("Can't allocate cmod hash table");
	}

	static const struct luaL_Reg top_methods[] = {
		{ "load",		lcmod_module_load	},
		{ NULL, NULL },
	};
	luaL_register_module(L, "cmod", top_methods);
	lua_pop(L, 1);

	static const struct luaL_Reg module_methods[] = {
		{ "load",		lcmod_func_load		},
		{ "unload",		lcmod_module_unload	},
		{ "__index",		lcmod_module_index	},
		{ "__serialize",	lcmod_module_serialize	},
		{ "__gc",		lcmod_module_gc		},
		{ NULL, NULL },
	};
	luaL_register_type(L, cmod_module_uname, module_methods);

	static const struct luaL_Reg func_methods[] = {
		{ "unload",		lcmod_func_unload	},
		{ "__index",		lcmod_func_index	},
		{ "__serialize",	lcmod_func_serialize	},
		{ "__call",		lcmod_func_call		},
		{ "__gc",		lcmod_func_gc		},
		{ NULL, NULL },
	};
	luaL_register_type(L, cmod_func_uname, func_methods);
}
