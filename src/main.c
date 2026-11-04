/*
 * main.c - command-line front end (FR8).
 * Only parses the command word and dispatches to the matching run_* function.
 */
#include "common.h"

typedef struct {
    const char *name;
    int (*handler)(int, char **);
    const char *syntax;
    const char *description;
} Command;

static const Command commands[] = {
    { "sched",  run_sched,  "sched <fcfs|rr|priority|compare> <jobs.txt> [quantum]",
                            "CPU scheduling: FCFS, Round Robin, Priority, or all three side by side" },
    { "logger", run_logger, "logger [--items N] [--buffer N] [--log FILE]",
                            "Producer-consumer event logger using pthreads" },
    { "ipc",    run_ipc,    "ipc <run|cleanup>",
                            "Shared memory + named semaphore between two processes" },
    { "banker", run_banker, "banker <state.txt>",
                            "Banker's safety algorithm on a resource state" },
    { "worker", run_worker, "worker <normal|fail|signal>",
                            "fork()/waitpid() worker; fail/signal are SIMULATED failures" },
    { "limits", run_limits, "limits <nofile|cpu|mem>",
                            "Demonstrate a setrlimit() resource limit in a child process" },
    { "demo",   run_demo,   "demo",
                            "Integrated demonstration of all features" },
};
#define NUM_COMMANDS ((int)(sizeof(commands) / sizeof(commands[0])))

static void print_usage_to(FILE *out)
{
    fprintf(out, "CampusCompute - job scheduling and resource management prototype\n\n");
    fprintf(out, "Usage: ./campuscompute <command> [arguments]\n\nCommands:\n");
    for (int i = 0; i < NUM_COMMANDS; i++)
        fprintf(out, "  %s\n      %s\n", commands[i].syntax, commands[i].description);
    fprintf(out, "  help\n      Show this message\n");
    fprintf(out, "\nExit codes: 0 = success, 1 = runtime/input error, 2 = usage error\n");
}

void print_usage(void)
{
    print_usage_to(stdout);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "error: no command given\n\n");
        print_usage_to(stderr);
        return RC_USAGE;
    }

    if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0 ||
        strcmp(argv[1], "-h") == 0) {
        print_usage();
        return RC_OK;
    }

    for (int i = 0; i < NUM_COMMANDS; i++) {
        if (strcmp(argv[1], commands[i].name) == 0) {
            /* hand over the arguments that come AFTER the command word */
            int rc = commands[i].handler(argc - 2, argv + 2);
            if (rc == RC_USAGE)
                fprintf(stderr, "usage: ./campuscompute %s\n", commands[i].syntax);
            return rc;
        }
    }

    fprintf(stderr, "error: unknown command '%s'\n\n", argv[1]);
    print_usage_to(stderr);
    return RC_USAGE;
}
