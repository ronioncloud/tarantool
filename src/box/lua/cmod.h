/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright 2010-2021, Tarantool AUTHORS, please see AUTHORS file.
 */

#pragma once

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

struct lua_State;

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
