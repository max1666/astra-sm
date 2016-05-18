/*
 * Astra Core (Main loop)
 * http://cesbo.com/astra
 *
 * Copyright (C) 2012-2013, Andrey Dyldin <and@cesbo.com>
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

#include <astra.h>
#include <core/mainloop.h>
#include <core/event.h>
#include <core/thread.h>
#include <core/timer.h>
#include <luaapi/luaapi.h>
#include <luaapi/state.h>

#define MSG(_msg) "[mainloop] " _msg

/* garbage collector interval */
#define LUA_GC_TIMEOUT (1 * 1000 * 1000)

/* maximum number of jobs queued */
#define JOB_QUEUE_SIZE 256

typedef struct
{
    loop_callback_t proc;
    void *arg;
    void *owner;
} loop_job_t;

typedef struct
{
    uint32_t flags;
    unsigned int stop_cnt;

    loop_job_t jobs[JOB_QUEUE_SIZE];
    unsigned int job_cnt;
    asc_mutex_t job_mutex;
} asc_main_loop_t;

static asc_main_loop_t *main_loop = NULL;

/*
 * callback queue
 */

/* add a procedure to main loop's job list */
void asc_job_queue(void *owner, loop_callback_t proc, void *arg)
{
    bool overflow = false;

    asc_mutex_lock(&main_loop->job_mutex);
    if (main_loop->job_cnt < JOB_QUEUE_SIZE)
    {
        loop_job_t *const job = &main_loop->jobs[main_loop->job_cnt++];

        job->proc = proc;
        job->arg = arg;
        job->owner = owner;
    }
    else
    {
        main_loop->job_cnt = 0;
        overflow = true;
    }
    asc_mutex_unlock(&main_loop->job_mutex);

    if (overflow)
        asc_log_error(MSG("job queue overflow, list flushed"));
}

/* remove jobs belonging to a specific module or object */
void asc_job_prune(void *owner)
{
    unsigned int i = 0;

    asc_mutex_lock(&main_loop->job_mutex);
    while (i < main_loop->job_cnt)
    {
        loop_job_t *const job = &main_loop->jobs[i];

        if (job->owner == owner)
        {
            main_loop->job_cnt--;
            memmove(job, &job[1], (main_loop->job_cnt - i) * sizeof(*job));
        }
        else
        {
            i++;
        }
    }
    asc_mutex_unlock(&main_loop->job_mutex);
}

/* run all queued callbacks */
static void run_jobs(void)
{
    loop_job_t *const first = &main_loop->jobs[0];
    loop_job_t job;

    asc_mutex_lock(&main_loop->job_mutex);
    while (main_loop->job_cnt > 0)
    {
        /* pull first job in queue */
        main_loop->job_cnt--;
        job = *first;
        memmove(first, &first[1], main_loop->job_cnt * sizeof(*first));

        /* run it with mutex unlocked */
        asc_mutex_unlock(&main_loop->job_mutex);
        job.proc(job.arg);
        asc_mutex_lock(&main_loop->job_mutex);
    }
    asc_mutex_unlock(&main_loop->job_mutex);
}

/*
 * event loop
 */
void asc_main_loop_init(void)
{
    main_loop = (asc_main_loop_t *)calloc(1, sizeof(*main_loop));
    asc_assert(main_loop != NULL, MSG("calloc() failed"));

    asc_mutex_init(&main_loop->job_mutex);
}

void asc_main_loop_destroy(void)
{
    asc_mutex_destroy(&main_loop->job_mutex);

    ASC_FREE(main_loop, free);
}

/* process events, return when a shutdown or reload is requested */
bool asc_main_loop_run(void)
{
    uint64_t current_time = asc_utime();
    uint64_t gc_check_timeout = current_time;
    unsigned int ev_sleep = 0;

    while (true)
    {
        asc_event_core_loop(ev_sleep);
        asc_timer_core_loop();
        asc_thread_core_loop();

        if (main_loop->flags)
        {
            const uint32_t flags = main_loop->flags;
            main_loop->flags = 0;

            if (flags & MAIN_LOOP_SHUTDOWN)
            {
                main_loop->stop_cnt = 0;
                return false;
            }
            else if (flags & MAIN_LOOP_RELOAD)
            {
                return true;
            }
            else if (flags & MAIN_LOOP_SIGHUP)
            {
                asc_log_reopen();

                lua_getglobal(lua, "on_sighup");
                if (lua_isfunction(lua, -1))
                    lua_call(lua, 0, 0);
                else
                    lua_pop(lua, 1);
            }
            else if (flags & MAIN_LOOP_NO_SLEEP)
            {
                ev_sleep = 0;
                continue;
            }
        }

        current_time = asc_utime();
        if ((current_time - gc_check_timeout) >= LUA_GC_TIMEOUT)
        {
            gc_check_timeout = current_time;
            lua_gc(lua, LUA_GCCOLLECT, 0);
        }

        run_jobs();

        ev_sleep = 1;
    }
}

/*
 * loop controls
 */
void asc_main_loop_set(uint32_t flag)
{
    main_loop->flags |= flag;
}

/* request graceful shutdown, abort if called multiple times */
void astra_shutdown(void)
{
    if (main_loop->flags & MAIN_LOOP_SHUTDOWN)
    {
        if (++main_loop->stop_cnt >= 3)
        {
            /*
             * NOTE: can't use regular exit() here as this is usually
             *       run by a signal handler thread. cleanup will try to
             *       join the thread on itself, possibly resulting in
             *       a deadlock.
             */
            _exit(EXIT_MAINLOOP);
        }
        else if (main_loop->stop_cnt >= 2)
        {
            asc_log_error(MSG("main thread appears to be blocked; "
                              "will abort on next shutdown request"));
        }
    }

    asc_main_loop_set(MAIN_LOOP_SHUTDOWN);
}
