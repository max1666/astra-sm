/*
 * Astra Lua Library (Str2Hex)
 * http://cesbo.com/astra
 *
 * Copyright (C) 2012-2013, Andrey Dyldin <and@cesbo.com>
 *                    2017, Artem Kharitonov <artem@3phase.pw>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Binary to hex and vice versa
 *
 * Methods:
 *      (string):hex()
 *                  - dump data in hex
 *      (string):bin()
 *                  - convert hex to binary
 */

#include <astra/astra.h>
#include <astra/utils/strhex.h>
#include <astra/luaapi/module.h>

static
int method_hex(lua_State *L)
{
    size_t data_size = 0;
    const void *const data = luaL_checklstring(L, 1, &data_size);

    luaL_Buffer b;
    char *const p = luaL_buffinitsize(L, &b, (data_size * 2) + 1);
    au_hex2str(p, data, data_size);
    luaL_addsize(&b, data_size * 2);
    luaL_pushresult(&b);

    return 1;
}

static
int method_bin(lua_State *L)
{
    size_t data_size = 0;
    const char *const data = luaL_checklstring(L, 1, &data_size);
    data_size /= 2;

    luaL_Buffer b;
    char *const p = luaL_buffinitsize(L, &b, data_size);
    au_str2hex(data, p, data_size);
    luaL_addsize(&b, data_size);
    luaL_pushresult(&b);

    return 1;
}

static
void module_load(lua_State *L)
{
    /* <string>:hex(), bin() */
    lua_getglobal(L, "string");
    lua_pushcfunction(L, method_hex);
    lua_setfield(L, -2, "hex");
    lua_pushcfunction(L, method_bin);
    lua_setfield(L, -2, "bin");
    lua_pop(L, 1); /* string */
}

BINDING_REGISTER(strhex)
{
    .load = module_load,
};
