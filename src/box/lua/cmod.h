/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2021, Tarantool AUTHORS, please see AUTHORS file.
 */

#pragma once

#include <sys/types.h>
#include <sys/stat.h>

#include "box/func_def.h"
#include "trivia/config.h"

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

struct lua_State;

/**
 * Attributes for cmod cache invalidation.
 */
struct cmod_attr {
#ifdef TARGET_OS_DARWIN
	struct timespec st_mtimespec;
#else
	struct timespec st_mtim;
#endif
	dev_t st_dev;
	ino_t st_ino;
	off_t st_size;
};

/**
 * Shared library module.
 */
struct cmod {
	/** Module dlhandle. */
	void *handle;
	/** Module ID. */
	int64_t id;
	/** Number of references. */
	int64_t refs;
	/** File attributes. */
	struct cmod_attr attr;
	/** Length of @a package. */
	size_t package_len;
	/** Path to the module. */
	char package[0];
};

/** Increase reference to cmod. */
void
cmod_ref(struct cmod *m);

/** Decrease reference to cmod and free it if last one. */
void
cmod_unref(struct cmod *m);

/**
 * Lookup for cmod entry in cache.
 *
 * @param package package name.
 * @param package_len length of @a package.
 *
 * @returns module pointer if found, NULL otherwise.
 */
struct cmod *
cmod_cache_find(const char *package, size_t package_len);

/**
 * Put new cmod entry to cache.
 *
 * @param cmod entry to put.
 *
 * @returns 0 on success, -1 otherwise (diag is set).
 */
int
cmod_cache_put(struct cmod *m);

/**
 * Allocate and load a new C module instance.
 *
 * Allocates a new C module instance, copies shared library
 * to a safe place, loads it and remove then leaving DSO purely
 * in memory. This is done because libc doesn't detect file
 * updates properly. The module get cached by putting it into
 * the modules hash.
 *
 * @param package package name.
 * @param package_len length of @a package.
 * @param source_path path to the shared library.
 *
 * @returns module pointer on succes, NULL otherwise, diag is set.
 */
struct cmod *
cmod_new(const char *package, size_t package_len, const char *source_path);

/**
 * Find package in Lua's "package.cpath".
 *
 * @param package package name.
 * @param package_len length of @package.
 * @param path resolved path.
 * @param path_len length of @a path.
 *
 * @return 0 on success, -1 otherwise (diag is set).
 */
int
cmod_find_package(const char *package, size_t package_len,
		  char *path, size_t path_len);

/**
 * Execute a function.
 *
 * @param m the module function sits in.
 * @param func_addr function address to call.
 * @args args incoming arguments.
 * @ret rets execution results.
 *
 * @return 0 on success, -1 otherwise(diag is set).
 */
int
cmod_call(struct cmod *m, box_function_f func_addr,
	  struct port *args, struct port *ret);

/**
 * Initialize cmod Lua module.
 *
 * @param L Lua state where to register the cmod module.
 */
void
box_lua_cmod_init(struct lua_State *L);

#if defined(__cplusplus)
}
#endif /* defined(__plusplus) */
