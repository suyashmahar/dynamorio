/* ****************************************************
 * Copyright (c) 2026 Arm Limited. All rights reserved.
 * Copyright (c) 2026 Meta Platforms, Inc.  All rights reserved.
 * ***************************************************/

/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of ARM Limited nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without
 *   specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL ARM LIMITED OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

/* This test case validates the behaviour enabled by the
 * -attach_unmask_suspend_signal option, which for internal attach, uses
 *  ptrace to take over threads masking SIGILL or are blocked in one of the
 *  sigwait syscalls. Two threads are created:
 * - sig_thread: all signals are masked and blocked and thread waits in
 *               sigwaitinfo().
 * - busy_thread: SIGILL is masked and the thread just runs without being
 *                blocked in any sigwait syscall. It verifies its mask while
 *                DynamoRIO is running and again after detach.
 */

#include "configure.h"
#include "dr_api.h"
#include "tools.h"
#include "condvar.h"
#include "thread.h"

#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#ifdef LINUX
#    include <sys/syscall.h>
#    include <unistd.h>
#endif

static void *signals_blocked;
static void *busy_started;
static pid_t busy_tid;

enum {
    BUSY_WAITING,
    BUSY_CHECK_WHILE_TRACING,
    BUSY_CHECKED_WHILE_TRACING,
    BUSY_CHECK_AFTER_DETACH,
};

static atomic_int busy_state;

static void
check_sigill_blocked(void)
{
    sigset_t mask;
    int res = pthread_sigmask(SIG_BLOCK, NULL, &mask);
    assert(res == 0);
    res = sigismember(&mask, SIGILL);
    assert(res == 1);
}

static THREAD_FUNC_RETURN_TYPE
sig_thread(void *arg)
{
    sigset_t set;
    siginfo_t info;
    int res;

    sigfillset(&set);
    res = pthread_sigmask(SIG_BLOCK, &set, NULL);
    assert(res == 0);

    print("sig_thread blocked signals\n");
    signal_cond_var(signals_blocked);

    while (true) {
        res = sigwaitinfo(&set, &info);
        if (res == -1) {
            if (errno == EINTR)
                continue;
        }
        assert(res != -1);
        if (res == SIGUSR1) {
            print("sig_thread received SIGUSR1\n");
            break;
        }
    }
    return NULL;
}

static THREAD_FUNC_RETURN_TYPE
busy_thread(void *arg)
{
    sigset_t mask;
    int res = sigemptyset(&mask);
    assert(res == 0);
    res = sigaddset(&mask, SIGILL);
    assert(res == 0);
    res = pthread_sigmask(SIG_BLOCK, &mask, NULL);
    assert(res == 0);

#ifdef LINUX
    busy_tid = (pid_t)syscall(SYS_gettid);
    print("busy_thread starting (tid=%d)\n", busy_tid);
#else
    print("busy_thread starting\n");
#endif
    signal_cond_var(busy_started);
    while (atomic_load(&busy_state) != BUSY_CHECK_WHILE_TRACING) {
        thread_yield();
    }
    check_sigill_blocked();
    atomic_store(&busy_state, BUSY_CHECKED_WHILE_TRACING);
    while (atomic_load(&busy_state) != BUSY_CHECK_AFTER_DETACH)
        thread_yield();
    check_sigill_blocked();
    print("busy_thread exiting\n");
    return NULL;
}

DR_EXPORT void
dr_client_main(client_id_t id, int argc, const char *argv[])
{
    print("in dr_client_main\n");
}

int
main(int argc, const char *argv[])
{
    signals_blocked = create_cond_var();
    busy_started = create_cond_var();

    print("starting busy_thread\n");
    thread_t busy = create_thread(busy_thread, NULL);
    wait_cond_var(busy_started);

    print("starting sig_thread\n");
    thread_t thread = create_thread(sig_thread, NULL);

    wait_cond_var(signals_blocked);

    print("pre-DR start\n");
    assert(!dr_app_running_under_dynamorio());
    /* This will hang sig_thread unless ptrace takeover is enabled using the
     * -attach_unmask_suspend_signal option.
     */
    dr_app_setup_and_start();
    assert(dr_app_running_under_dynamorio());

    atomic_store(&busy_state, BUSY_CHECK_WHILE_TRACING);
    while (atomic_load(&busy_state) != BUSY_CHECKED_WHILE_TRACING)
        thread_yield();

    print("sending SIGUSR1 to sig_thread\n");
    pthread_kill(thread, SIGUSR1);
    join_thread(thread);

    print("pre-DR stop\n");
    dr_app_stop_and_cleanup();
    assert(!dr_app_running_under_dynamorio());

    print("stopping busy_thread\n");
    atomic_store(&busy_state, BUSY_CHECK_AFTER_DETACH);
    join_thread(busy);

    destroy_cond_var(signals_blocked);
    destroy_cond_var(busy_started);

    print("all done\n");
    return 0;
}
