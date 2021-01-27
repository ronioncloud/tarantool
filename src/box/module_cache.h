/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2021, Tarantool AUTHORS, please see AUTHORS file.
 */

#pragma once

#include "small/rlist.h"

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */


/**
 * API of C stored function.
 */

struct port;

struct box_function_ctx {
	struct port *port;
};

typedef struct box_function_ctx box_function_ctx_t;
typedef int (*box_function_f)(box_function_ctx_t *ctx,
			      const char *args,
			      const char *args_end);

/**
 * Dynamic shared module.
 */
struct module {
	/**
	 * Module dlhandle.
	 */
	void *handle;
	/**
	 * List of associated symbols (functions).
	 */
	struct rlist funcs_list;
	/**
	 * Count of active references to the module.
	 */
	int64_t refs;
	/**
	 * Module's package name.
	 */
	char package[0];
};

/**
 * Callable symbol bound to a module.
 */
struct module_sym {
	/**
	 * Anchor for module::funcs_list.
	 */
	struct rlist item;
	/**
	 * For C functions, address of the function.
	 */
	box_function_f addr;
	/**
	 * A module the symbol belongs to.
	 */
	struct module *module;
	/**
	 * Symbol (function) name definition.
	 */
	char *name;
};

/**
 * Load a new module symbol.
 *
 * @param mod_sym symbol to load.
 *
 * @returns 0 on succse, -1 otherwise, diag is set.
 */
int
module_sym_load(struct module_sym *mod_sym);

/**
 * Unload a module's symbol.
 *
 * @param mod_sym symbol to unload.
 */
void
module_sym_unload(struct module_sym *mod_sym);

/**
 * Execute a module symbol (run a function).
 *
 * The function packs function arguments into a message pack
 * and send it as a function argument. Function may return
 * results via @a ret stream.
 *
 * @param mod_sym module symbol to run.
 * @param args function arguments.
 * @param[out] ret execution results.
 *
 * @returns 0 on success, -1 otherwise, diag is set.
 */
int
module_sym_call(struct module_sym *mod_sym, struct port *args,
		struct port *ret);

/**
 * Reload a module and all associated symbols.
 *
 * @param package shared library path start.
 * @param package_end shared library path end.
 *
 * @return 0 on succes, -1 otherwise, diag is set.
 */
int
module_reload(const char *package, const char *package_end);

/**
 * Initialize modules subsystem.
 *
 * @return 0 on succes, -1 otherwise, diag is set.
 */
int
module_init(void);

/**
 * Free modules subsystem.
 */
void
module_free(void);

#if defined(__cplusplus)
}
#endif /* defined(__plusplus) */
