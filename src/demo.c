/*
 * demo.c - integrated demonstration (owner: Person B).
 *
 * Usage: demo
 * Runs every feature in order by calling the same run_* entry points the
 * individual commands use, then verifies cleanup (child processes reaped,
 * extra threads gone, IPC objects removed, log file written) and prints a
 * summary table. Run it from the project root so the files in tests/ can be found.
 */
#include "common.h"

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_ARGS 4
#define JOBS_FILE         "tests/jobs.txt"
#define BANKER_SAFE_FILE  "tests/banker_safe.txt"
#define BANKER_UNSAFE_FILE "tests/banker_unsafe.txt"

typedef struct {
    int  (*handler)(int, char **);
    const char *cmd;                 /* command word */
    int   argc;                      /* arguments after the command word */
    char *argv[MAX_ARGS];
    int   accept_err;                /* 1: RC_ERR also counts as a pass */
    const char *what;                /* one-line explanation */
} Step;

static const Step STEPS[] = {
    { run_sched,  "sched",  2, { "fcfs", JOBS_FILE }, 0,
      "FCFS scheduling (FR2)" },
    { run_sched,  "sched",  2, { "rr", JOBS_FILE }, 0,
      "Round Robin scheduling, default quantum (FR2)" },
    { run_sched,  "sched",  2, { "priority", JOBS_FILE }, 0,
      "Non-preemptive priority scheduling (FR2)" },
    { run_sched,  "sched",  2, { "compare", JOBS_FILE }, 0,
      "All three policies compared on the same job set (FR2)" },
    { run_banker, "banker", 1, { BANKER_SAFE_FILE }, 0,
      "Banker's algorithm: SAFE state with a safe sequence (FR5)" },
    /* Person A may return RC_OK or RC_ERR for an unsafe state: both accepted.
       Tighten this once Person A's banker.c return value is confirmed. */
    { run_banker, "banker", 1, { BANKER_UNSAFE_FILE }, 1,
      "Banker's algorithm: UNSAFE state identified (FR5)" },
    { run_logger, "logger", 4, { "--items", "3", "--buffer", "2" }, 0,
      "Pthreads producer-consumer logger writing campuscompute.log (FR3)" },
    { run_ipc,    "ipc",    1, { "run" }, 0,
      "Shared memory + named semaphores between two processes (FR4)" },
    { run_worker, "worker", 1, { "normal" }, 0,
      "Worker process completes normally (FR6)" },
    { run_worker, "worker", 1, { "fail" }, 0,
      "[SIMULATED] worker failure with non-zero exit status (FR6)" },
    { run_worker, "worker", 1, { "signal" }, 0,
      "[SIMULATED] worker killed by a signal (FR6)" },
    { run_limits, "limits", 1, { "nofile" }, 0,
      "setrlimit: open-file limit (FR7)" },
    { run_limits, "limits", 1, { "cpu" }, 0,
      "setrlimit: CPU-time limit (FR7)" },
    { run_limits, "limits", 1, { "mem" }, 0,
      "setrlimit: address-space limit (FR7)" },
};
#define NSTEPS ((int)(sizeof(STEPS) / sizeof(STEPS[0])))

static void command_line(const Step *s, char *out, size_t n)
{
    snprintf(out, n, "%s", s->cmd);
    for (int i = 0; i < s->argc; i++) {
        size_t len = strlen(out);
        snprintf(out + len, n - len, " %s", s->argv[i]);
    }
}

static int count_threads(void)          /* threads in this process, -1 if unknown */
{
    DIR *d = opendir("/proc/self/task");
    if (d == NULL)
        return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (e->d_name[0] != '.')
            n++;
    closedir(d);
    return n;
}

static int count_lines(const char *path, long *bytes)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    *bytes = (long)st.st_size;
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return -1;
    int lines = 0, c;
    while ((c = fgetc(f)) != EOF)
        if (c == '\n')
            lines++;
    fclose(f);
    return lines;
}

int run_demo(int argc, char **argv)
{
    (void)argv;
    if (argc != 0)
        return RC_USAGE;

    const char *needed[] = { JOBS_FILE, BANKER_SAFE_FILE, BANKER_UNSAFE_FILE };
    for (int i = 0; i < 3; i++) {
        if (access(needed[i], R_OK) != 0) {
            fprintf(stderr, "error: cannot read %s (%s). Run the demo from the "
                    "project root directory.\n", needed[i], strerror(errno));
            return RC_ERR;
        }
    }

    printf("==============================================================\n");
    printf(" CampusCompute - integrated demonstration\n");
    printf("==============================================================\n");
    printf("Conventions used throughout:\n");
    printf("  Tie-break : earlier arrival first, then lower job ID\n");
    printf("  Priority  : lower number = higher priority, non-preemptive\n");
    printf("  RR        : default quantum 4; arrivals during a slice queue before the preempted job\n");
    printf("  Metrics   : TAT = CT - AT, WT = TAT - BT\n");
    printf("  IPC       : only objects with the %s prefix are unlinked\n", IPC_PREFIX);
    printf("  Failures  : simulated failures are always tagged [SIMULATED]\n");
    printf("Steps: %d\n", NSTEPS);

    int pass[NSTEPS], rcs[NSTEPS], npass = 0;
    char cmdline[NSTEPS][96];

    for (int i = 0; i < NSTEPS; i++) {
        const Step *s = &STEPS[i];
        command_line(s, cmdline[i], sizeof cmdline[i]);
        printf("\n==================== STEP %d/%d: %s ====================\n",
               i + 1, NSTEPS, s->what);
        printf("$ ./campuscompute %s\n\n", cmdline[i]);
        fflush(stdout);

        char *args[MAX_ARGS + 1];
        for (int k = 0; k < s->argc; k++)
            args[k] = s->argv[k];
        args[s->argc] = NULL;

        int rc = s->handler(s->argc, args);
        fflush(stdout);
        rcs[i] = rc;
        pass[i] = (rc == RC_OK) || (s->accept_err && rc == RC_ERR);
        if (pass[i]) npass++;
        printf("\n>> STEP %d %s (exit code %d)\n", i + 1, pass[i] ? "PASSED" : "FAILED", rc);
        fflush(stdout);
    }

    /* ---- cleanup verification ------------------------------------------ */
    printf("\n==================== CLEANUP VERIFICATION ====================\n");
    int clean_ok = 1;

    errno = 0;
    pid_t left = waitpid(-1, NULL, WNOHANG);
    if (left == -1 && errno == ECHILD)
        printf("Processes : all child processes have been reaped (none left)\n");
    else {
        printf("Processes : ERROR - an unreaped child exists (pid %d)\n", (int)left);
        clean_ok = 0;
    }

    int threads = count_threads();
    if (threads == 1)
        printf("Threads   : only the main thread remains (logger threads joined)\n");
    else if (threads > 1) {
        printf("Threads   : ERROR - %d threads still running\n", threads);
        clean_ok = 0;
    } else
        printf("Threads   : could not check (/proc not available)\n");

    printf("IPC       : running 'ipc cleanup' to confirm nothing is left:\n");
    fflush(stdout);
    char *cl[] = { "cleanup" };
    if (run_ipc(1, cl) != RC_OK)
        clean_ok = 0;

    long bytes = 0;
    int lines = count_lines(DEFAULT_LOG_FILE, &bytes);
    if (lines >= 0)
        printf("Log file  : %s exists (%d lines, %ld bytes)\n", DEFAULT_LOG_FILE, lines, bytes);
    else {
        printf("Log file  : ERROR - %s was not created\n", DEFAULT_LOG_FILE);
        clean_ok = 0;
    }

    /* ---- summary -------------------------------------------------------- */
    printf("\n==================== SUMMARY ====================\n");
    printf(" %-3s %-36s %-8s %s\n", "#", "Command", "Result", "Exit");
    for (int i = 0; i < NSTEPS; i++)
        printf(" %-3d %-36s %-8s %d\n", i + 1, cmdline[i], pass[i] ? "PASS" : "FAIL", rcs[i]);
    printf("\nFeatures passed : %d / %d\n", npass, NSTEPS);
    printf("Cleanup         : %s\n", clean_ok ? "OK" : "PROBLEM FOUND");
    int ok = (npass == NSTEPS) && clean_ok;
    printf("Overall         : %s\n", ok ? "SUCCESS" : "FAILURE");
    return ok ? RC_OK : RC_ERR;
}
