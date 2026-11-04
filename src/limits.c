/*
 * limits.c - resource limits with setrlimit() (FR7, owner: Person B).
 *
 * Usage: limits <nofile|cpu|mem>
 * Each mode forks a child, the child lowers one limit with setrlimit() and
 * then deliberately exceeds it. The parent waitpid()s and reports the result.
 * The limit is applied only in the child, so the parent is never affected.
 *   nofile : RLIMIT_NOFILE -> open() fails with EMFILE
 *   cpu    : RLIMIT_CPU    -> kernel sends SIGXCPU, child is terminated
 *   mem    : RLIMIT_AS     -> malloc() returns NULL (ENOMEM)
 */
#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define NOFILE_LIMIT   16UL                    /* max open file descriptors  */
#define CPU_LIMIT_SEC  1UL                     /* max CPU seconds (soft)     */
#define MEM_LIMIT_MB   64UL                    /* max address space in MB    */
#define MEM_CHUNK_MB   4UL                     /* size of each malloc        */

static int set_limit(int resource, const char *name, rlim_t soft, rlim_t hard)
{
    struct rlimit rl = { .rlim_cur = soft, .rlim_max = hard };
    if (setrlimit(resource, &rl) != 0) {
        perror("setrlimit");
        return -1;
    }
    struct rlimit check;
    if (getrlimit(resource, &check) == 0)
        printf("[child] %s limit set: soft=%lu hard=%lu\n", name,
               (unsigned long)check.rlim_cur, (unsigned long)check.rlim_max);
    fflush(stdout);
    return 0;
}

/* --- child workloads: each exits RC_OK if the limit was enforced -------- */

static void child_nofile(void)
{
    if (set_limit(RLIMIT_NOFILE, "RLIMIT_NOFILE", NOFILE_LIMIT, NOFILE_LIMIT) != 0)
        _exit(RC_ERR);

    int opened = 0;
    for (;;) {
        int fd = open("/dev/null", O_RDONLY);
        if (fd < 0) {
            printf("[child] [SIMULATED] limit hit: open() failed after %d extra "
                   "files: %s (errno=%d)\n", opened, strerror(errno), errno);
            fflush(stdout);
            _exit(errno == EMFILE ? RC_OK : RC_ERR);
        }
        opened++;
        if (opened > 1000) {                    /* safety: limit not enforced */
            printf("[child] limit NOT enforced\n");
            fflush(stdout);
            _exit(RC_ERR);
        }
    }
}

static void child_cpu(void)
{
    /* soft limit => SIGXCPU; hard limit a bit higher => SIGKILL backstop */
    if (set_limit(RLIMIT_CPU, "RLIMIT_CPU(s)", CPU_LIMIT_SEC, CPU_LIMIT_SEC + 2) != 0)
        _exit(RC_ERR);

    printf("[child] [SIMULATED] busy loop - will exceed %lu s of CPU time...\n",
           CPU_LIMIT_SEC);
    fflush(stdout);
    alarm(15);                                  /* safety net, never normally hit */
    volatile unsigned long long x = 0;
    for (;;)
        x++;
}

static void child_mem(void)
{
    if (set_limit(RLIMIT_AS, "RLIMIT_AS(bytes)", MEM_LIMIT_MB * 1024 * 1024,
                  MEM_LIMIT_MB * 1024 * 1024) != 0)
        _exit(RC_ERR);

    size_t chunk = MEM_CHUNK_MB * 1024 * 1024;
    void *blocks[64];
    int n = 0;
    while (n < 64) {
        void *p = malloc(chunk);
        if (p == NULL) {
            printf("[child] [SIMULATED] limit hit: malloc(%lu MB) failed after "
                   "%d MB allocated: %s (errno=%d)\n", MEM_CHUNK_MB,
                   (int)(n * MEM_CHUNK_MB), strerror(errno), errno);
            fflush(stdout);
            for (int i = 0; i < n; i++)
                free(blocks[i]);
            _exit(errno == ENOMEM ? RC_OK : RC_ERR);
        }
        blocks[n++] = p;
    }
    printf("[child] limit NOT enforced\n");
    fflush(stdout);
    _exit(RC_ERR);
}

int run_limits(int argc, char **argv)
{
    if (argc != 1)
        return RC_USAGE;

    const char *mode = argv[0];
    void (*child_fn)(void) = NULL;
    if (strcmp(mode, "nofile") == 0)      child_fn = child_nofile;
    else if (strcmp(mode, "cpu") == 0)    child_fn = child_cpu;
    else if (strcmp(mode, "mem") == 0)    child_fn = child_mem;
    else {
        fprintf(stderr, "error: unknown limits mode '%s'\n", mode);
        return RC_USAGE;
    }

    printf("[parent pid=%d] starting limits test (mode: %s)\n", (int)getpid(), mode);
    fflush(stdout);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return RC_ERR;
    }
    if (pid == 0) {
        child_fn();
        _exit(RC_ERR);                          /* not reached */
    }

    int status = 0;
    pid_t w;
    do {
        w = waitpid(pid, &status, 0);
    } while (w < 0 && errno == EINTR);
    if (w < 0) {
        perror("waitpid");
        return RC_ERR;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == RC_OK) {
        printf("[parent] child %d exited normally: the %s limit was enforced "
               "by the kernel\n", (int)pid, mode);
        return RC_OK;
    }
    if (WIFSIGNALED(status) && WTERMSIG(status) == SIGXCPU && strcmp(mode, "cpu") == 0) {
        printf("[parent] [SIMULATED] child %d was killed by signal %d (%s): "
               "the cpu limit was enforced by the kernel\n",
               (int)pid, WTERMSIG(status), strsignal(WTERMSIG(status)));
        return RC_OK;
    }
    if (WIFSIGNALED(status))
        printf("[parent] child %d killed by signal %d (%s) - unexpected\n",
               (int)pid, WTERMSIG(status), strsignal(WTERMSIG(status)));
    else
        printf("[parent] child %d exited with status %d - limit not demonstrated\n",
               (int)pid, WEXITSTATUS(status));
    return RC_ERR;
}
