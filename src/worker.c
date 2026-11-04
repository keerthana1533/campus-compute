/*
 * worker.c - fork()/waitpid() worker processes (FR6, owner: Person B).
 *
 * Usage: worker <normal|fail|signal>
 *   normal : child does a little "work" and exits with status 0.
 *   fail   : SIMULATED failure - child exits with WORKER_FAIL_EXIT (non-zero).
 *   signal : SIMULATED crash   - child kills itself with SIGKILL.
 * The parent waits with waitpid() and reports exactly how the child ended.
 */
#include "common.h"

#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define WORKER_FAIL_EXIT 3   /* agreed failure exit code for the simulated failure */

/* Code that runs only in the child. Never returns. */
static void child_main(const char *mode)
{
    printf("[worker pid=%d] started (mode: %s)\n", (int)getpid(), mode);
    fflush(stdout);
    usleep(200 * 1000);                       /* pretend to compute for 0.2 s */

    if (strcmp(mode, "fail") == 0) {
        printf("[worker pid=%d] [SIMULATED] failure: exiting with status %d\n",
               (int)getpid(), WORKER_FAIL_EXIT);
        fflush(stdout);
        _exit(WORKER_FAIL_EXIT);
    }
    if (strcmp(mode, "signal") == 0) {
        printf("[worker pid=%d] [SIMULATED] crash: sending myself SIGKILL\n",
               (int)getpid());
        fflush(stdout);
        raise(SIGKILL);
        _exit(RC_ERR);                        /* not reached */
    }

    printf("[worker pid=%d] work finished successfully\n", (int)getpid());
    fflush(stdout);
    _exit(0);                                 /* _exit: don't re-flush parent's buffers */
}

/* Parent side: wait for the child and describe how it ended. */
static int report_status(pid_t pid, int status, int simulated)
{
    const char *tag = simulated ? "[SIMULATED] " : "";

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == 0)
            printf("[parent] child %d finished NORMALLY (exit status 0)\n", (int)pid);
        else
            printf("[parent] %sFAILURE DETECTED: child %d exited with non-zero status %d\n",
                   tag, (int)pid, code);
    } else if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        printf("[parent] %sFAILURE DETECTED: child %d was killed by signal %d (%s)\n",
               tag, (int)pid, sig, strsignal(sig));
    } else {
        printf("[parent] child %d ended in an unexpected state (raw status 0x%x)\n",
               (int)pid, status);
        return RC_ERR;
    }
    return RC_OK;
}

int run_worker(int argc, char **argv)
{
    if (argc != 1)
        return RC_USAGE;

    const char *mode = argv[0];
    if (strcmp(mode, "normal") != 0 && strcmp(mode, "fail") != 0 &&
        strcmp(mode, "signal") != 0) {
        fprintf(stderr, "error: unknown worker mode '%s'\n", mode);
        return RC_USAGE;
    }
    int simulated = (strcmp(mode, "normal") != 0);

    printf("[parent pid=%d] creating worker (mode: %s)\n", (int)getpid(), mode);
    fflush(stdout);                           /* avoid the child inheriting unflushed output */

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return RC_ERR;
    }
    if (pid == 0)
        child_main(mode);

    int status = 0;
    pid_t w;
    do {
        w = waitpid(pid, &status, 0);
    } while (w < 0 && errno == EINTR);        /* retry if interrupted by a signal */
    if (w < 0) {
        perror("waitpid");
        return RC_ERR;
    }

    return report_status(pid, status, simulated);
}
