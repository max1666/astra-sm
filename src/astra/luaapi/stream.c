/*
 * Astra Lua API (Stream Module)
 * http://cesbo.com/astra
 *
 * Copyright (C) 2012-2015, Andrey Dyldin <and@cesbo.com>
 *               2015-2016, Artem Kharitonov <artem@3phase.pw>
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

#include <astra/astra.h>
#include <astra/luaapi/stream.h>
#include <astra/core/list.h>

#define MSG(_msg) "[stream %s] " _msg, \
    (mod->manifest != NULL ? mod->manifest->name : NULL)

typedef struct module_stream_t module_stream_t;

struct module_stream_t
{
    module_data_t *self;
    module_stream_t *parent;

    stream_callback_t on_ts;
    asc_list_t *children;

    demux_callback_t join_pid;
    demux_callback_t leave_pid;
    uint8_t pid_list[TS_MAX_PIDS];
};

struct module_data_t
{
    /*
     * NOTE: data structs in all stream modules MUST begin with the
     *       following members. Use STREAM_MODULE_DATA() macro when
     *       defining stream module structs to add appropriate size
     *       padding.
     */
    const module_manifest_t *manifest;
    lua_State *lua;
    module_stream_t *stream;
};

ASC_STATIC_ASSERT(sizeof(module_data_t) <= STREAM_MODULE_DATA_SIZE);

/*
 * init and cleanup
 */

static
int method_set_upstream(lua_State *L, module_data_t *mod);

static
int method_stream(lua_State *L, module_data_t *mod)
{
    lua_pushlightuserdata(L, mod);
    return 1;
}

static
const module_method_t stream_methods[] =
{
    { "set_upstream", method_set_upstream },
    { "stream", method_stream },
    { NULL, NULL },
};

void module_stream_init(lua_State *L, module_data_t *mod
                        , stream_callback_t on_ts)
{
    ASC_ASSERT(mod->stream == NULL, MSG("module already initialized"));
    module_stream_t *const st = ASC_ALLOC(1, module_stream_t);

    st->self = mod;
    st->on_ts = on_ts;
    st->children = asc_list_init();

    /* demux default: forward downstream pid requests to parent */
    st->join_pid = module_demux_join;
    st->leave_pid = module_demux_leave;

    mod->stream = st;

    if (L != NULL)
    {
        module_add_methods(L, mod, stream_methods);

        if (lua_istable(L, MODULE_OPTIONS_IDX))
        {
            lua_getfield(L, MODULE_OPTIONS_IDX, "upstream");
            if (!lua_isnil(L, -1))
                method_set_upstream(L, mod);

            lua_pop(L, 1);
        }
    }
}

void module_stream_destroy(module_data_t *mod)
{
    if (mod->stream == NULL)
        /* not initialized */
        return;

    /* leave all joined pids */
    for (unsigned int i = 0; i < TS_MAX_PIDS; i++)
    {
        while (module_demux_check(mod, i))
            module_demux_leave(mod, i);
    }

    /* detach from upstream */
    module_stream_attach(NULL, mod);

    /* detach children */
    asc_list_for(mod->stream->children)
    {
        module_stream_t *const i =
            (module_stream_t *)asc_list_data(mod->stream->children);

        i->parent = NULL;
    }

    ASC_FREE(mod->stream->children, asc_list_destroy);
    ASC_FREE(mod->stream, free);
}

/*
 * streaming module tree
 */

static
int method_set_upstream(lua_State *L, module_data_t *mod)
{
    module_data_t *up = NULL;

    switch (lua_type(L, -1))
    {
        case LUA_TNIL:
            module_stream_attach(NULL, mod);
            break;

        case LUA_TLIGHTUSERDATA:
            if (mod->stream->on_ts == NULL)
                luaL_error(L, MSG("this module cannot receive TS"));

            up = (module_data_t *)lua_touserdata(L, -1);
            module_stream_attach(up, mod);
            break;

        default:
            luaL_error(L, MSG("option 'upstream' requires a stream module"));
    }

    return 0;
}

void module_stream_attach(module_data_t *mod, module_data_t *child)
{
    /* save pid membership data, leave all pids */
    uint8_t saved_list[TS_MAX_PIDS] = { 0 };
    for (unsigned int i = 0; i < TS_MAX_PIDS; i++)
    {
        while (module_demux_check(child, i))
        {
            module_demux_leave(child, i);
            saved_list[i]++;
        }
    }

    /* switch parents */
    module_stream_t *const cs = child->stream;

    if (cs->parent != NULL)
    {
        asc_list_remove_item(cs->parent->children, cs);
        cs->parent = NULL;
    }

    if (mod != NULL)
    {
        module_stream_t *const ps = mod->stream;
        ASC_ASSERT(ps != NULL, MSG("attaching to uninitialized module"));
        ASC_ASSERT(cs->on_ts != NULL, MSG("this module cannot receive TS"));

        cs->parent = ps;
        asc_list_insert_tail(ps->children, cs);
    }

    /* re-request pids from new parent */
    for (unsigned int i = 0; i < TS_MAX_PIDS; i++)
    {
        while (saved_list[i]-- > 0)
            module_demux_join(child, i);
    }
}

void module_stream_send(void *arg, const uint8_t *ts)
{
    module_data_t *const mod = (module_data_t *)arg;

    asc_list_for(mod->stream->children)
    {
        module_stream_t *const i =
            (module_stream_t *)asc_list_data(mod->stream->children);

        i->on_ts(i->self, ts);
    }
}

/*
 * pid membership
 */

void module_demux_set(module_data_t *mod, demux_callback_t join_pid
                      , demux_callback_t leave_pid)
{
    mod->stream->join_pid = join_pid;
    mod->stream->leave_pid = leave_pid;
}

void module_demux_join(module_data_t *mod, uint16_t pid)
{
    ASC_ASSERT(ts_pid_valid(pid), MSG("join: pid %hu out of range"), pid);
    module_stream_t *const st = mod->stream;

    ++st->pid_list[pid];
    if (st->pid_list[pid] == 1 && st->parent != NULL
        && st->parent->join_pid != NULL)
    {
        st->parent->join_pid(st->parent->self, pid);
    }
}

void module_demux_leave(module_data_t *mod, uint16_t pid)
{
    ASC_ASSERT(ts_pid_valid(pid), MSG("leave: pid %hu out of range"), pid);
    module_stream_t *const st = mod->stream;

    if (st->pid_list[pid] > 0)
    {
        --st->pid_list[pid];
        if (st->pid_list[pid] == 0 && st->parent != NULL
            && st->parent->leave_pid != NULL)
        {
            st->parent->leave_pid(st->parent->self, pid);
        }
    }
    else
    {
        asc_log_error(MSG("double leave on pid %hu"), pid);
    }
}

bool module_demux_check(const module_data_t *mod, uint16_t pid)
{
    ASC_ASSERT(ts_pid_valid(pid), MSG("check: pid %hu out of range"), pid);
    return (mod->stream->pid_list[pid] > 0);
}
