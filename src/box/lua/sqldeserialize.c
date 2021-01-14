#include "diag.h"
#include "execute.h"
#include "lua/utils.h"
#include "sqlInt.h"
#include "../sql/vdbe.h"	// FIXME
#include "../sql/vdbeInt.h"	// FIXME
#include "../execute.h"		// FIXME
#include "../schema.h"		// FIXME
#include "../session.h"		// FIXME
#include "../box.h"		// FIXME
#include <small/ibuf.h>
#include <msgpuck/msgpuck.h>

#include "sqlparser.h"

#include <assert.h>
#include <stdlib.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

extern uint32_t CTID_STRUCT_SQL_PARSED_AST;

#define ON_(str, expected, len) \
	if (strncmp(expected, str, len) == 0)

#define EXPECT_MAP(data) ({ \
		enum mp_type type = mp_typeof(**data); \
		assert(type == MP_MAP); \
		uint32_t size = mp_decode_map(data); \
		size; })

#define EXPECT_ARRAY(data) ({ \
		enum mp_type type = mp_typeof(**data); \
		assert(type == MP_ARRAY); \
		uint32_t size = mp_decode_array(data); \
		size; })

#define IN_S(data, v, len) { \
		enum mp_type type = mp_typeof(**data); \
		assert(type == MP_STR || type == MP_NIL); \
		if (type == MP_STR) { \
			len = 0; \
			v = mp_decode_str(data, &len); \
		} else if (type == MP_STR) { \
			v = NULL; \
			len = 0; \
			mp_decode_nil(data); \
		} \
	}

#define EXPECT_KEY(data, v, len) \
		IN_S(data, v, len)

#define IN_V(data, p, f, type) { \
		ON_(key, #f, len) { \
			/* assert(type == MP_STR); */ \
			p->f = mp_decode_##type(data); \
		} \
	}

#define IN_VS(data, p, f) \
		ON_(key, #f, len) { \
			const char * ps; \
			uint32_t len = 0; \
			IN_S(data, ps, len); \
			if (ps != NULL) strncpy(p->f, ps, len); \
			else p->f[0] = '\0'; \
		}

#define MATCH_K(data, key)

static int64_t
mp_decode_Xint(const char **data)
{
	switch (mp_typeof(**data)) {
	case MP_UINT:
		return mp_decode_uint(data);
	case MP_INT:
		return mp_decode_int(data);
	default:
		assert(0);
	}
	return 0;
}

static void
mp_decode_select(const char **data)
{
	uint32_t size = EXPECT_MAP(data);
	const char * key;
	uint32_t len = 0;
	IN_S(data, key, len);

	struct Select s = {0};
	ON_(key, "select", len) {
		for (uint32_t i = 0; i < size; i++) {
			int n_selects = EXPECT_ARRAY(data);
			for (int j = 0; j < n_selects; j++) {
				struct Select *p = &s;

				int n = EXPECT_MAP(data);
				for (int k = 0; k < n; k++) {
					uint32_t len = 0;
					EXPECT_KEY(data, key, len);

					IN_V(data, p, op, uint);
					IN_V(data, p, nSelectRow, Xint);
					IN_V(data, p, selFlags, uint);
					IN_V(data, p, iLimit, Xint);
					IN_V(data, p, iOffset, Xint);
					IN_VS(data, p, zSelName);
					IN_V(data, p, addrOpenEphm[0], Xint);
					IN_V(data, p, addrOpenEphm[1], Xint);
					MATCH_K(data, "expr") {
						// rc = mp_decode_select_expr(cfg, data);
					}
					MATCH_K(data, "from") {
						// rc = mp_decode_select_from(cfg, data);
					}
				}
				// FIXME - connect s.prior
			}
		}
	}
}

static int
sqlparser_msgpack_decode_string(struct lua_State *L, bool check)
{
	size_t data_len;
	const char *data = lua_tolstring(L, 1, &data_len);
	const char *p = data;
	if (check) {
		if (mp_check(&p, data + data_len) != 0)
			return luaL_error(L, "sqldeserialize: invalid MsgPack");
	}
	p = data;
	mp_decode_select(&p);
	lua_pushinteger(L, p - data + 1);
	return 2;

}

int
lbox_sqlparser_deserialize(struct lua_State *L)
{
	int index = lua_gettop(L);
	int type = index >= 1 ? lua_type(L, 1) : LUA_TNONE;
	switch (type) {
	case LUA_TSTRING:
		return sqlparser_msgpack_decode_string(L, true);
	default:
		return luaL_error(L, "sqldeserialize: "
				  "a Lua string or 'char *' expected");
	}
	return 1;
}
