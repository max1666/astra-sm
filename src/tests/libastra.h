/*
 * Astra Unit Tests
 * http://cesbo.com/astra
 *
 * Copyright (C) 2015-2016, Artem Kharitonov <artem@3phase.pw>
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
#include <check.h>

#ifndef _UNIT_TESTS_H_
#define _UNIT_TESTS_H_ 1

extern enum fork_status can_fork;

/* test setup and teardown */
unsigned int get_timer_res(void);
bool is_fd_inherited(int fd);
void lib_setup(void);
void lib_teardown(void);

/* core */
Suite *core_alloc(void);
Suite *core_assert(void);
Suite *core_clock(void);
Suite *core_compat(void);
Suite *core_event(void);
Suite *core_list(void);
Suite *core_log(void);
Suite *core_mainloop(void);
Suite *core_spawn(void);
Suite *core_child(void);
Suite *core_thread(void);
Suite *core_timer(void);

/* luaapi */
Suite *luaapi_luaapi(void);
Suite *luaapi_module(void);
Suite *luaapi_state(void);
Suite *luaapi_stream(void);

/* lualib */
Suite *lualib_astra(void);
Suite *lualib_base64(void);
Suite *lualib_json(void);
Suite *lualib_md5(void);
Suite *lualib_pidfile(void);
Suite *lualib_rc4(void);
Suite *lualib_sha1(void);
Suite *lualib_strhex(void);
Suite *lualib_utils(void);

/* mpegts */
Suite *mpegts_mpegts(void);
Suite *mpegts_pcr(void);
Suite *mpegts_sync(void);

/* utils */
Suite *utils_base64(void);
Suite *utils_crc32b(void);
Suite *utils_crc8(void);
Suite *utils_json(void);
Suite *utils_md5(void);
Suite *utils_rc4(void);
Suite *utils_sha1(void);
Suite *utils_strhex(void);

/* unit test list */
typedef Suite (*(*const suite_func_t)(void));

static suite_func_t suite_list[] = {
    /* core */
    core_alloc,
    core_assert,
    core_clock,
    core_compat,
    core_event,
    core_list,
    core_log,
    core_mainloop,
    core_spawn,
    core_child,
    core_thread,
    core_timer,

    /* luaapi */
    luaapi_luaapi,
    luaapi_module,
    luaapi_state,
    luaapi_stream,

    /* lualib */
    lualib_astra,
    lualib_base64,
    lualib_json,
    lualib_md5,
    lualib_pidfile,
    lualib_rc4,
    lualib_sha1,
    lualib_strhex,
    lualib_utils,

    /* mpegts */
    mpegts_mpegts,
    mpegts_pcr,
    mpegts_sync,

    /* utils */
    utils_base64,
    utils_crc32b,
    utils_crc8,
    utils_json,
    utils_md5,
    utils_rc4,
    utils_sha1,
    utils_strhex,

    NULL,
};

#endif /* _UNIT_TESTS_H_ */
