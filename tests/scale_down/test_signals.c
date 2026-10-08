/* SPDX-License-Identifier: Apache-2.0
 * Real processes/pthreads/signals; only the NF packet loop and cleanup are mocked.
 */
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
#include "spawn.inc"

_Static_assert(ATOMIC_SHORT_LOCK_FREE == 2, "Signal handler needs lock-free atomics");
typedef _Atomic short rte_atomic16_t;
static int interrupt_read;
void onvm_nflib_handle_signal(int sig);
static short rte_atomic16_read(rte_atomic16_t *value) {
    short n = atomic_load(value);
    if (interrupt_read) { interrupt_read = 0; onvm_nflib_handle_signal(SIGTERM); }
    return n;
}
static void rte_atomic16_set(rte_atomic16_t *value, short n) { atomic_store(value, n); }
struct onvm_nf_local_ctx;
struct functions { int (*user_actions)(struct onvm_nf_local_ctx *); };
struct onvm_nf {
    struct { rte_atomic16_t *sleep_state; sem_t *nf_mutex; } shared_core;
    struct functions *function_table;
};
struct onvm_nf_local_ctx {
    rte_atomic16_t keep_running, nf_init_finished;
    struct onvm_nf *nf;
};
typedef void (*handle_signal_func)(int);
static struct onvm_nf_local_ctx *main_nf_local_ctx;
static handle_signal_func global_nf_signal_handler;
static int ONVM_NF_SHARE_CORES, report_fd, stubborn;
static volatile sig_atomic_t handled;

static void rte_exit(int status, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));
static void rte_exit(int status, const char *fmt, ...) {
    va_list args; va_start(args, fmt); vfprintf(stderr, fmt, args); va_end(args);
    exit(status);
}
static void received(int sig) { handled = sig; }
static void check_mask(int blocked) {
    sigset_t mask;
    assert(!pthread_sigmask(SIG_SETMASK, NULL, &mask));
    assert(sigismember(&mask, SIGINT) == blocked && sigismember(&mask, SIGTERM) == blocked);
    assert(sigismember(&mask, SIGUSR1) == blocked); /* Unrelated mask is preserved too. */
}
static void *onvm_nflib_thread_main_loop(void *arg) {
    struct onvm_nf_local_ctx *ctx = arg;
    check_mask(1);
    if (ctx != main_nf_local_ctx) return NULL;
    assert(write(report_fd, "R", 1) == 1);
    while (stubborn || rte_atomic16_read(&ctx->keep_running)) usleep(1000);
    check_mask(1);
    return NULL;
}
#include "signals.inc"

static int callback_result;
static int callback(struct onvm_nf_local_ctx *ctx) { (void)ctx; return callback_result; }
static void run_callback(struct onvm_nf_local_ctx *nf_local_ctx) {
    struct onvm_nf *nf = nf_local_ctx->nf;
#define ONVM_NO_CALLBACK NULL
#include "callback.inc"
}

static int child(int fd, int ignore_stop) {
    alarm(5); /* A failed regression must not leave a spinning child behind. */
    check_mask(0);
    assert(getpgrp() == getpid());
    report_fd = fd; stubborn = ignore_stop;
    struct onvm_nf nf = {0};
    struct onvm_nf_local_ctx ctx = {.keep_running = 1, .nf_init_finished = 1, .nf = &nf};
    assert(!onvm_nflib_start_signal_handler(&ctx, received));
    sigset_t mask;
    sigemptyset(&mask); sigaddset(&mask, SIGINT); sigaddset(&mask, SIGTERM); sigaddset(&mask, SIGUSR1);
    assert(!pthread_sigmask(SIG_BLOCK, &mask, NULL)); /* Model onvm_nflib_start_nf(). */

    /* An ONVM child NF must not unblock the calling thread's pending signal. */
    struct onvm_nf_local_ctx other = {.keep_running = 1};
    assert(!pthread_kill(pthread_self(), SIGTERM));
    assert(!onvm_nflib_run(&other));
    check_mask(1); assert(!handled && rte_atomic16_read(&ctx.keep_running));
    sigset_t pending; sigemptyset(&pending); sigaddset(&pending, SIGTERM);
    int sig; assert(!sigwait(&pending, &sig) && sig == SIGTERM);

    struct functions callbacks = {.user_actions = callback}; nf.function_table = &callbacks;
    /* Reproduce a signal between the old callback path's flag read and write. */
    interrupt_read = 1;
    run_callback(&ctx);
    if (interrupt_read) { interrupt_read = 0; onvm_nflib_handle_signal(SIGTERM); }
    assert(!rte_atomic16_read(&ctx.keep_running));
    callback_result = 1; rte_atomic16_set(&ctx.keep_running, 1);
    run_callback(&ctx); assert(!rte_atomic16_read(&ctx.keep_running));
    callback_result = 0; rte_atomic16_set(&ctx.keep_running, 1); handled = 0;
    run_callback(&ctx); assert(rte_atomic16_read(&ctx.keep_running));

    assert(!onvm_nflib_run(&ctx));
    check_mask(1);
    assert(handled == SIGTERM || handled == SIGINT);
    assert(!rte_atomic16_read(&ctx.keep_running));
    assert(write(report_fd, "C", 1) == 1); /* Normal cleanup after the packet thread has joined. */
    close(report_fd);
    return 0;
}

static int wait_child(pid_t pid, int *status, unsigned timeout_ms) {
    for (unsigned i = 0; i < timeout_ms; i++) {
        pid_t rc = waitpid(pid, status, WNOHANG);
        if (rc == pid) return 1;
        assert(rc == 0 || errno == EINTR);
        usleep(1000);
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 3) return child(atoi(argv[1]), atoi(argv[2]));
    sigset_t mask, saved;
    sigemptyset(&mask); sigaddset(&mask, SIGINT); sigaddset(&mask, SIGTERM); sigaddset(&mask, SIGUSR1);
    assert(!pthread_sigmask(SIG_BLOCK, &mask, &saved)); /* UPF-C's spawning thread. */
    for (int scenario = 0; scenario < 3; scenario++) {
        int pipefd[2]; assert(!pipe(pipefd));
        char fd[24]; snprintf(fd, sizeof(fd), "%d", pipefd[1]);
        char *args[] = {argv[0], fd, scenario == 2 ? "1" : "0", NULL};
        pid_t pid; assert(!spawn_worker(&pid, args)); close(pipefd[1]);
        check_mask(1); /* Spawn did not alter the parent's mask. */
        struct pollfd ready = {.fd = pipefd[0], .events = POLLIN};
        int available = poll(&ready, 1, 3000);
        char marker; int status;
        if (available <= 0 || read(pipefd[0], &marker, 1) != 1 || marker != 'R') {
            kill(-pid, SIGKILL); waitpid(pid, &status, 0); assert(0 && "Child failed to reach packet loop");
        }
        assert(!kill(-pid, scenario == 1 ? SIGINT : SIGTERM));
        int exited = wait_child(pid, &status, scenario == 2 ? 100 : 2000);
        if (!exited) { assert(!kill(-pid, SIGKILL)); assert(waitpid(pid, &status, 0) == pid); }
        if (scenario == 2) {
            assert(!exited && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
            assert(read(pipefd[0], &marker, 1) == 0); /* Forced exit skipped normal cleanup. */
        } else {
            assert(exited && WIFEXITED(status) && WEXITSTATUS(status) == 0);
            assert(read(pipefd[0], &marker, 1) == 1 && marker == 'C');
            assert(read(pipefd[0], &marker, 1) == 0);
        }
        close(pipefd[0]);
    }
    assert(!pthread_sigmask(SIG_SETMASK, &saved, NULL));
    puts("PASS: real spawn masks, SIGTERM/SIGINT, blocked NF threads, persistent stop requests and forced exit");
    return 0;
}
