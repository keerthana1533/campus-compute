/*
 * sched.c - CPU scheduling simulator (FR2).
 *
 *   ./campuscompute sched <fcfs|rr|priority|compare> <jobs.txt> [quantum]
 *
 * Three policies, one shared timeline + metrics engine:
 *   FCFS      - non-preemptive, runs jobs in arrival order
 *   RR        - preemptive, fixed time quantum, FIFO ready queue
 *   PRIORITY  - non-preemptive, lowest priority NUMBER runs first
 *
 * Time is simulated (integer time units); nothing here sleeps or runs real work.
 *
 * Tie-breaking rule used EVERYWHERE: earlier arrival first, then lower job ID.
 */
#include "common.h"
#include <errno.h>
#include <limits.h>

#define IDLE_ID      (-1)     /* pseudo job ID used for "CPU idle" timeline segments */
#define WRAP_WIDTH   76       /* wrap long timeline lines so they fit a terminal */
#define WRAP_INDENT  4
#define QUANTUM_MAX  1000000L

typedef enum { P_FCFS, P_RR, P_PRIORITY } Policy;

/* One block of the Gantt chart: job_id ran (or CPU idled) from start to end. */
typedef struct { int job_id; int start; int end; } Segment;

/* Growable list of segments (RR with a small quantum can produce many). */
typedef struct { Segment *seg; int count; int cap; } Timeline;

/* Totals used for the averages and the comparison table. */
typedef struct { long total_ct, total_tat, total_wt; int idle_time; int length; } Summary;

/* ------------------------------------------------------------------ */
/* Timeline helpers                                                    */
/* ------------------------------------------------------------------ */

static void timeline_free(Timeline *tl)
{
    free(tl->seg);
    tl->seg = NULL;
    tl->count = tl->cap = 0;
}

/* Append a segment. If it directly continues the previous one for the same job
 * (e.g. RR where only one job is left), merge them. Returns 0 or -1 (out of memory). */
static int timeline_add(Timeline *tl, int job_id, int start, int end)
{
    if (tl->count > 0) {
        Segment *last = &tl->seg[tl->count - 1];
        if (last->job_id == job_id && last->end == start) {
            last->end = end;
            return 0;
        }
    }
    if (tl->count == tl->cap) {
        int new_cap = tl->cap ? tl->cap * 2 : 16;
        Segment *p = realloc(tl->seg, (size_t)new_cap * sizeof(Segment));
        if (!p) {
            fprintf(stderr, "error: out of memory while building the timeline\n");
            return -1;
        }
        tl->seg = p;
        tl->cap = new_cap;
    }
    tl->seg[tl->count].job_id = job_id;
    tl->seg[tl->count].start  = start;
    tl->seg[tl->count].end    = end;
    tl->count++;
    return 0;
}

/* Print text, starting a new indented line first if it would run past WRAP_WIDTH. */
static void wrap_emit(int *col, const char *text)
{
    int len = (int)strlen(text);
    if (*col > WRAP_INDENT && *col + len > WRAP_WIDTH) {
        printf("\n%*s", WRAP_INDENT, "");
        *col = WRAP_INDENT;
    }
    printf("%s", text);
    *col += len;
}

/* "[J1 0-5] [J2 5-8] [IDLE 8-10] ..." */
static void print_timeline(const Timeline *tl)
{
    char item[48];
    int col = WRAP_INDENT;
    printf("Timeline (job start-end):\n%*s", WRAP_INDENT, "");
    for (int i = 0; i < tl->count; i++) {
        if (tl->seg[i].job_id == IDLE_ID)
            snprintf(item, sizeof item, "[IDLE %d-%d] ", tl->seg[i].start, tl->seg[i].end);
        else
            snprintf(item, sizeof item, "[J%d %d-%d] ", tl->seg[i].job_id,
                     tl->seg[i].start, tl->seg[i].end);
        wrap_emit(&col, item);
    }
    printf("\n");
}

/* "J1 -> J2 -> IDLE -> J3" (used by the comparison table) */
static void print_order(const Timeline *tl)
{
    char item[32];
    int col = WRAP_INDENT;
    printf("%*s", WRAP_INDENT, "");
    for (int i = 0; i < tl->count; i++) {
        const char *arrow = (i + 1 < tl->count) ? " -> " : "";
        if (tl->seg[i].job_id == IDLE_ID)
            snprintf(item, sizeof item, "IDLE%s", arrow);
        else
            snprintf(item, sizeof item, "J%d%s", tl->seg[i].job_id, arrow);
        wrap_emit(&col, item);
    }
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Job helpers                                                         */
/* ------------------------------------------------------------------ */

/* Tie-break order: earlier arrival first, then lower job ID. */
static int comes_first(const Job *a, const Job *b)
{
    if (a->arrival != b->arrival)
        return a->arrival < b->arrival;
    return a->id < b->id;
}

/* Priority order: lower priority number first; ties use comes_first(). */
static int better_priority(const Job *a, const Job *b)
{
    if (a->priority != b->priority)
        return a->priority < b->priority;
    return comes_first(a, b);
}

/* order[] = job indices sorted by (arrival, id). Insertion sort: n <= MAX_JOBS. */
static void sort_by_arrival(const Job *jobs, int n, int *order)
{
    for (int i = 0; i < n; i++)
        order[i] = i;
    for (int i = 1; i < n; i++) {
        int key = order[i];
        int j = i - 1;
        while (j >= 0 && comes_first(&jobs[key], &jobs[order[j]])) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }
}

/* Lifecycle step NEW -> READY for every job that has arrived by 'time'. */
static void mark_arrived(Job *jobs, int n, int time)
{
    for (int i = 0; i < n; i++)
        if (jobs[i].status == JOB_NEW && jobs[i].arrival <= time)
            jobs[i].status = JOB_READY;
}

/* Lifecycle step RUNNING -> DONE and the metrics: TAT = CT - AT, WT = TAT - BT. */
static void finish_job(Job *j, int time)
{
    j->completion = time;
    j->turnaround = j->completion - j->arrival;
    j->waiting    = j->turnaround - j->burst;
    j->remaining  = 0;
    j->status     = JOB_DONE;
}

/* ------------------------------------------------------------------ */
/* The three algorithms. Each fills jobs[] metrics and the timeline.   */
/* Return RC_OK or RC_ERR (out of memory).                             */
/* ------------------------------------------------------------------ */

/* FCFS: sort by (arrival, id), run each job to completion in that order. */
static int sched_fcfs(Job *jobs, int n, Timeline *tl)
{
    int order[MAX_JOBS];
    int time = 0;

    sort_by_arrival(jobs, n, order);
    for (int k = 0; k < n; k++) {
        Job *j = &jobs[order[k]];
        if (time < j->arrival) {                       /* CPU idle until this job arrives */
            if (timeline_add(tl, IDLE_ID, time, j->arrival) != 0) return RC_ERR;
            time = j->arrival;
        }
        mark_arrived(jobs, n, time);
        j->start  = time;
        j->status = JOB_RUNNING;
        if (timeline_add(tl, j->id, time, time + j->burst) != 0) return RC_ERR;
        time += j->burst;
        finish_job(j, time);
    }
    return RC_OK;
}

/* Non-preemptive priority: whenever the CPU is free, pick the arrived job with the lowest
 * priority number (ties: earlier arrival, then lower ID). If 'verbose', log each decision. */
static int sched_priority(Job *jobs, int n, Timeline *tl, int verbose)
{
    int time = 0, done = 0;

    while (done < n) {
        mark_arrived(jobs, n, time);

        int best = -1;
        for (int i = 0; i < n; i++)
            if (jobs[i].status == JOB_READY &&
                (best < 0 || better_priority(&jobs[i], &jobs[best])))
                best = i;

        if (best < 0) {                                /* nobody ready: idle until next arrival */
            int next = INT_MAX;
            for (int i = 0; i < n; i++)
                if (jobs[i].status == JOB_NEW && jobs[i].arrival < next)
                    next = jobs[i].arrival;
            if (timeline_add(tl, IDLE_ID, time, next) != 0) return RC_ERR;
            time = next;
            continue;
        }

        if (verbose) {
            int tied = 0;
            printf("  t=%-3d ready:", time);
            for (int i = 0; i < n; i++) {
                if (jobs[i].status != JOB_READY) continue;
                printf(" J%d(p%d)", jobs[i].id, jobs[i].priority);
                if (jobs[i].priority == jobs[best].priority) tied++;
            }
            printf("  -> run J%d", jobs[best].id);
            if (tied > 1)
                printf("   [tie on priority %d: earlier arrival, then lower ID]", jobs[best].priority);
            printf("\n");
        }

        Job *j = &jobs[best];
        j->start  = time;
        j->status = JOB_RUNNING;
        if (timeline_add(tl, j->id, time, time + j->burst) != 0) return RC_ERR;
        time += j->burst;
        finish_job(j, time);
        done++;
    }
    return RC_OK;
}

/* Admit jobs whose arrival time has been reached into the circular ready queue. */
static void rr_admit(Job *jobs, int n, const int *order, int *next,
                     int time, int *queue, int head, int *count)
{
    while (*next < n && jobs[order[*next]].arrival <= time) {
        int idx = order[*next];
        jobs[idx].status = JOB_READY;
        queue[(head + *count) % n] = idx;
        (*count)++;
        (*next)++;
    }
}

/* Round Robin: FIFO ready queue, each job runs for min(quantum, remaining).
 * Jobs that arrive during a slice (or exactly when it ends) are queued BEFORE the
 * preempted job is put back at the tail. */
static int sched_rr(Job *jobs, int n, int quantum, Timeline *tl)
{
    int order[MAX_JOBS], queue[MAX_JOBS];
    int head = 0, count = 0, next = 0, time = 0, done = 0;

    sort_by_arrival(jobs, n, order);
    rr_admit(jobs, n, order, &next, time, queue, head, &count);

    while (done < n) {
        if (count == 0) {                              /* queue empty: idle until next arrival */
            int arrive = jobs[order[next]].arrival;
            if (timeline_add(tl, IDLE_ID, time, arrive) != 0) return RC_ERR;
            time = arrive;
            rr_admit(jobs, n, order, &next, time, queue, head, &count);
            continue;
        }

        int idx = queue[head];                         /* dequeue the job at the head */
        head = (head + 1) % n;
        count--;

        Job *j = &jobs[idx];
        if (j->start < 0) j->start = time;             /* first time on the CPU */
        j->status = JOB_RUNNING;

        int slice = (j->remaining < quantum) ? j->remaining : quantum;
        if (timeline_add(tl, j->id, time, time + slice) != 0) return RC_ERR;
        time += slice;
        j->remaining -= slice;

        rr_admit(jobs, n, order, &next, time, queue, head, &count);   /* new arrivals first */

        if (j->remaining == 0) {
            finish_job(j, time);
            done++;
        } else {                                       /* preempted: back to the tail */
            j->status = JOB_READY;
            queue[(head + count) % n] = idx;
            count++;
        }
    }
    return RC_OK;
}

/* ------------------------------------------------------------------ */
/* Reporting                                                           */
/* ------------------------------------------------------------------ */

static void summarize(const Job *jobs, int n, const Timeline *tl, Summary *s)
{
    memset(s, 0, sizeof *s);
    for (int i = 0; i < n; i++) {
        s->total_ct  += jobs[i].completion;
        s->total_tat += jobs[i].turnaround;
        s->total_wt  += jobs[i].waiting;
    }
    for (int i = 0; i < tl->count; i++)
        if (tl->seg[i].job_id == IDLE_ID)
            s->idle_time += tl->seg[i].end - tl->seg[i].start;
    s->length = tl->count ? tl->seg[tl->count - 1].end : 0;
}

static void print_metrics(const Job *jobs, int n, const Summary *s)
{
    printf("\nMetrics (TAT = CT - AT, WT = TAT - BT):\n");
    printf("%-6s %-8s %-6s %-9s %-6s %-11s %-11s %-8s %-6s\n", "JobID", "Arrival", "Burst",
           "Priority", "Start", "Completion", "Turnaround", "Waiting", "Status");
    for (int i = 0; i < n; i++)
        printf("J%-5d %-8d %-6d %-9d %-6d %-11d %-11d %-8d %-6s\n", jobs[i].id, jobs[i].arrival,
               jobs[i].burst, jobs[i].priority, jobs[i].start, jobs[i].completion,
               jobs[i].turnaround, jobs[i].waiting, job_status_str(jobs[i].status));
    printf("\nAverage completion time : %.2f\n", (double)s->total_ct  / n);
    printf("Average turnaround time : %.2f\n",   (double)s->total_tat / n);
    printf("Average waiting time    : %.2f\n",   (double)s->total_wt  / n);
    printf("CPU idle time           : %d   (total schedule length: %d)\n", s->idle_time, s->length);
}

static void policy_label(Policy p, int quantum, char *buf, size_t size)
{
    switch (p) {
    case P_FCFS:     snprintf(buf, size, "FCFS");                        break;
    case P_RR:       snprintf(buf, size, "Round Robin (q=%d)", quantum); break;
    case P_PRIORITY: snprintf(buf, size, "Priority (non-preemptive)");   break;
    }
}

/* Run one policy on jobs[] (state is reset first). */
static int simulate(Policy p, Job *jobs, int n, int quantum, Timeline *tl, int verbose)
{
    jobs_reset_state(jobs, n);
    switch (p) {
    case P_FCFS:     return sched_fcfs(jobs, n, tl);
    case P_RR:       return sched_rr(jobs, n, quantum, tl);
    case P_PRIORITY: return sched_priority(jobs, n, tl, verbose);
    }
    return RC_ERR;
}

/* sched fcfs|rr|priority: header, rules, input, run, timeline, metrics. */
static int report_single(Policy p, Job *jobs, int n, int quantum)
{
    char label[48];
    Timeline tl = {0};
    Summary s;

    policy_label(p, quantum, label, sizeof label);
    printf("=== %s ===\n", label);
    switch (p) {
    case P_FCFS:
        printf("Rule: jobs run in arrival order, each to completion.\n");
        printf("Tie-break: equal arrival -> lower job ID first.\n");
        break;
    case P_RR:
        printf("Time quantum: %d\n", quantum);
        printf("Rule: FIFO ready queue; a job runs for min(quantum, remaining time), then goes to the tail.\n");
        printf("Tie-break: jobs arriving during a slice are queued before the preempted job; "
               "equal arrival -> lower job ID first.\n");
        break;
    case P_PRIORITY:
        printf("Rule: lower priority number = higher priority; non-preemptive.\n");
        printf("Tie-break: equal priority -> earlier arrival first, then lower job ID.\n");
        break;
    }

    printf("\nInput jobs:\n");
    jobs_reset_state(jobs, n);
    print_job_table(jobs, n);

    if (p == P_PRIORITY)
        printf("\nSelection log:\n");      /* simulate() prints one line per decision */
    int rc = simulate(p, jobs, n, quantum, &tl, 1);
    if (rc != RC_OK) {
        timeline_free(&tl);
        return rc;
    }

    printf("\n");
    print_timeline(&tl);
    summarize(jobs, n, &tl, &s);
    print_metrics(jobs, n, &s);
    timeline_free(&tl);
    return RC_OK;
}

/* sched compare: run all three on the same job set and show them side by side. */
static int report_compare(Job *jobs, int n, int quantum)
{
    const Policy policies[3] = { P_FCFS, P_RR, P_PRIORITY };
    Timeline tl[3] = { {0}, {0}, {0} };
    Summary  sum[3];
    char     label[3][48];
    int      rc = RC_OK;

    printf("=== Scheduling comparison (same job set, RR quantum = %d) ===\n", quantum);
    printf("Tie-break (all policies): earlier arrival first, then lower job ID. "
           "Priority: lower number = higher priority.\n\n");
    printf("Input jobs:\n");
    jobs_reset_state(jobs, n);
    print_job_table(jobs, n);

    for (int k = 0; k < 3 && rc == RC_OK; k++) {
        policy_label(policies[k], quantum, label[k], sizeof label[k]);
        rc = simulate(policies[k], jobs, n, quantum, &tl[k], 0);
        if (rc == RC_OK)
            summarize(jobs, n, &tl[k], &sum[k]);
    }

    if (rc == RC_OK) {
        printf("\n%-26s %-9s %-9s %-9s %-6s\n", "Policy", "Avg CT", "Avg TAT", "Avg WT", "Idle");
        for (int k = 0; k < 3; k++)
            printf("%-26s %-9.2f %-9.2f %-9.2f %-6d\n", label[k], (double)sum[k].total_ct / n,
                   (double)sum[k].total_tat / n, (double)sum[k].total_wt / n, sum[k].idle_time);

        /* lowest average waiting time (compare the integer totals, not the floats) */
        long best = sum[0].total_wt;
        for (int k = 1; k < 3; k++)
            if (sum[k].total_wt < best) best = sum[k].total_wt;
        printf("\nLowest average waiting time (%.2f):", (double)best / n);
        for (int k = 0; k < 3; k++)
            if (sum[k].total_wt == best) printf(" %s;", label[k]);
        printf("\n");

        printf("\nExecution order per policy:\n");
        for (int k = 0; k < 3; k++) {
            printf("  %s:\n", label[k]);
            print_order(&tl[k]);
        }
    }

    for (int k = 0; k < 3; k++)
        timeline_free(&tl[k]);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int run_sched(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "error: expected a policy and a job file (and an optional quantum)\n");
        return RC_USAGE;
    }

    const char *name = argv[0];
    const char *path = argv[1];
    int is_compare = 0;
    Policy policy = P_FCFS;

    if      (strcmp(name, "fcfs") == 0)     policy = P_FCFS;
    else if (strcmp(name, "rr") == 0)       policy = P_RR;
    else if (strcmp(name, "priority") == 0) policy = P_PRIORITY;
    else if (strcmp(name, "compare") == 0)  is_compare = 1;
    else {
        fprintf(stderr, "error: unknown scheduling policy '%s'\n", name);
        return RC_USAGE;
    }

    int quantum = RR_QUANTUM_DEFAULT;
    if (argc == 3) {
        if (!is_compare && policy != P_RR) {
            fprintf(stderr, "error: a quantum only applies to 'rr' and 'compare'\n");
            return RC_USAGE;
        }
        char *end = NULL;
        errno = 0;
        long q = strtol(argv[2], &end, 10);
        if (errno != 0 || end == argv[2] || *end != '\0' || q < 1 || q > QUANTUM_MAX) {
            fprintf(stderr, "error: quantum must be an integer between 1 and %ld (got '%s')\n",
                    QUANTUM_MAX, argv[2]);
            return RC_USAGE;
        }
        quantum = (int)q;
    }

    Job jobs[MAX_JOBS];
    int n = 0;
    int rc = load_jobs(path, jobs, &n);
    if (rc != RC_OK)
        return rc;

    return is_compare ? report_compare(jobs, n, quantum)
                      : report_single(policy, jobs, n, quantum);
}
