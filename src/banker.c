/*
 * banker.c - Banker's safety algorithm (FR5).
 *
 *   ./campuscompute banker <state.txt>
 *
 * State file (numbers separated by spaces/newlines; '#' starts a comment):
 *   n m                      number of processes, number of resource types
 *   Available                m numbers
 *   Allocation               n rows of m numbers
 *   Maximum                  n rows of m numbers
 *
 * Need = Maximum - Allocation. The safety algorithm looks for an order in which
 * every process can run to completion with the resources that are free at that moment.
 */
#include "common.h"
#include <errno.h>

#define MAX_PROC   MAX_JOBS     /* 32 processes */
#define MAX_RES    10           /* resource types A..J */
#define MAX_TOKENS (2 + MAX_RES + 2 * MAX_PROC * MAX_RES)
#define LINE_LEN   256
#define VALUE_MAX  1000000L

typedef struct {
    int n, m;
    int avail[MAX_RES];
    int alloc[MAX_PROC][MAX_RES];
    int max[MAX_PROC][MAX_RES];
    int need[MAX_PROC][MAX_RES];
} State;

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

/* Read every integer in the file into vals[]. Rejects non-numbers, negatives, huge values. */
static int read_numbers(const char *path, long *vals, int *count)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "error: cannot open state file '%s': %s\n", path, strerror(errno));
        return RC_ERR;
    }

    char line[LINE_LEN];
    int lineno = 0;
    *count = 0;

    while (fgets(line, sizeof line, fp)) {
        lineno++;
        if (!strchr(line, '\n') && !feof(fp)) {
            fprintf(stderr, "error: %s:%d: line too long\n", path, lineno);
            fclose(fp);
            return RC_ERR;
        }
        char *hash = strchr(line, '#');           /* drop comments */
        if (hash) *hash = '\0';

        char *p = line;
        for (;;) {
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
            if (*p == '\0') break;

            char *end = NULL;
            errno = 0;
            long v = strtol(p, &end, 10);
            if (end == p || (*end != '\0' && *end != ' ' && *end != '\t' &&
                             *end != '\r' && *end != '\n')) {
                int tok = (int)strcspn(p, " \t\r\n");          /* show just the offending word */
                fprintf(stderr, "error: %s:%d: '%.*s' is not a valid integer\n",
                        path, lineno, tok > 12 ? 12 : tok, p);
                fclose(fp);
                return RC_ERR;
            }
            if (errno != 0 || v < 0 || v > VALUE_MAX) {
                fprintf(stderr, "error: %s:%d: values must be between 0 and %ld\n",
                        path, lineno, VALUE_MAX);
                fclose(fp);
                return RC_ERR;
            }
            if (*count >= MAX_TOKENS) {
                fprintf(stderr, "error: %s: too many numbers in the file\n", path);
                fclose(fp);
                return RC_ERR;
            }
            vals[(*count)++] = v;
            p = end;
        }
    }
    fclose(fp);
    return RC_OK;
}

/* Turn the number list into a State and compute Need. */
static int load_state(const char *path, State *s)
{
    long vals[MAX_TOKENS];
    int count;

    if (read_numbers(path, vals, &count) != RC_OK)
        return RC_ERR;
    if (count < 2) {
        fprintf(stderr, "error: %s: expected 'n m' on the first line\n", path);
        return RC_ERR;
    }

    long n = vals[0], m = vals[1];
    if (n < 1 || n > MAX_PROC || m < 1 || m > MAX_RES) {
        fprintf(stderr, "error: %s: need 1..%d processes and 1..%d resource types (got n=%ld, m=%ld)\n",
                path, MAX_PROC, MAX_RES, n, m);
        return RC_ERR;
    }
    long expected = 2 + m + 2 * n * m;
    if (count != expected) {
        fprintf(stderr, "error: %s: for n=%ld, m=%ld expected %ld numbers "
                "(Available + Allocation + Maximum) but found %d\n", path, n, m, expected, count);
        return RC_ERR;
    }

    s->n = (int)n;
    s->m = (int)m;
    int k = 2;
    for (int j = 0; j < s->m; j++)
        s->avail[j] = (int)vals[k++];
    for (int i = 0; i < s->n; i++)
        for (int j = 0; j < s->m; j++)
            s->alloc[i][j] = (int)vals[k++];
    for (int i = 0; i < s->n; i++)
        for (int j = 0; j < s->m; j++)
            s->max[i][j] = (int)vals[k++];

    for (int i = 0; i < s->n; i++) {
        for (int j = 0; j < s->m; j++) {
            s->need[i][j] = s->max[i][j] - s->alloc[i][j];     /* Need = Maximum - Allocation */
            if (s->need[i][j] < 0) {
                fprintf(stderr, "error: %s: P%d holds more of resource %c (%d) than its maximum (%d)\n",
                        path, i, 'A' + j, s->alloc[i][j], s->max[i][j]);
                return RC_ERR;
            }
        }
    }
    return RC_OK;
}

/* ------------------------------------------------------------------ */
/* Output helpers                                                      */
/* ------------------------------------------------------------------ */

/* Format a vector as "[a b c]" into buf. */
static void vec_str(const int *v, int m, char *buf, size_t size)
{
    size_t off = 0;
    off += (size_t)snprintf(buf + off, size - off, "[");
    for (int j = 0; j < m && off < size; j++)
        off += (size_t)snprintf(buf + off, size - off, j ? " %d" : "%d", v[j]);
    if (off < size)
        snprintf(buf + off, size - off, "]");
}

static void print_state(const char *path, const State *s)
{
    char a[160], b[160], c[160];

    printf("=== Banker's safety algorithm ===\n");
    printf("State file: %s  (%d processes, %d resource types)\n\n", path, s->n, s->m);

    printf("Resource types:");
    for (int j = 0; j < s->m; j++)
        printf(" %c", 'A' + j);
    printf("\n");

    int total[MAX_RES] = {0};
    for (int j = 0; j < s->m; j++) {
        total[j] = s->avail[j];
        for (int i = 0; i < s->n; i++)
            total[j] += s->alloc[i][j];
    }
    vec_str(total, s->m, a, sizeof a);
    printf("Total instances (Available + all Allocation): %s\n", a);
    vec_str(s->avail, s->m, a, sizeof a);
    printf("Available                                   : %s\n\n", a);

    printf("%-6s %-16s %-16s %-16s\n", "Proc", "Allocation", "Maximum", "Need (Max-Alloc)");
    for (int i = 0; i < s->n; i++) {
        vec_str(s->alloc[i], s->m, a, sizeof a);
        vec_str(s->max[i],   s->m, b, sizeof b);
        vec_str(s->need[i],  s->m, c, sizeof c);
        printf("P%-5d %-16s %-16s %-16s\n", i, a, b, c);
    }
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* The safety algorithm                                                */
/* ------------------------------------------------------------------ */

/* Returns 1 if every resource in need[] is <= the matching resource in work[]. */
static int need_fits(const int *need, const int *work, int m)
{
    for (int j = 0; j < m; j++)
        if (need[j] > work[j])
            return 0;
    return 1;
}

/*
 * Work = Available; Finish[i] = false for all i.
 * Repeatedly scan the processes in order; if Finish[i] is false and Need[i] <= Work,
 * pretend P_i runs to completion and releases everything: Work += Allocation[i],
 * Finish[i] = true. Stop when a whole pass finds nothing.
 * Safe iff every process finished; the order they finished in is a safe sequence.
 */
static int is_safe(const State *s, int *seq, int *finished_count, int *finish, int *work_out)
{
    int work[MAX_RES];
    char w[160], nd[160], w2[160];
    int step = 0, progress;

    memcpy(work, s->avail, sizeof(int) * (size_t)s->m);
    memset(finish, 0, sizeof(int) * (size_t)s->n);
    *finished_count = 0;

    vec_str(work, s->m, w, sizeof w);
    printf("Safety algorithm trace:\n  Work = Available = %s\n", w);

    do {
        progress = 0;
        for (int i = 0; i < s->n; i++) {
            if (finish[i] || !need_fits(s->need[i], work, s->m))
                continue;

            vec_str(s->need[i], s->m, nd, sizeof nd);
            vec_str(work, s->m, w, sizeof w);
            for (int j = 0; j < s->m; j++)
                work[j] += s->alloc[i][j];               /* P_i finishes and releases its resources */
            vec_str(work, s->m, w2, sizeof w2);

            finish[i] = 1;
            seq[(*finished_count)++] = i;
            progress = 1;
            printf("  Step %d: P%d can finish (Need %s <= Work %s) -> Work = %s\n",
                   ++step, i, nd, w, w2);
        }
    } while (progress && *finished_count < s->n);

    memcpy(work_out, work, sizeof(int) * (size_t)s->m);
    return *finished_count == s->n;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int run_banker(int argc, char **argv)
{
    if (argc != 1) {
        fprintf(stderr, "error: expected exactly one argument: the state file\n");
        return RC_USAGE;
    }

    State s;
    if (load_state(argv[0], &s) != RC_OK)
        return RC_ERR;

    print_state(argv[0], &s);

    int seq[MAX_PROC], finish[MAX_PROC], done, work[MAX_RES];
    int safe = is_safe(&s, seq, &done, finish, work);

    printf("\n");
    if (safe) {
        printf("RESULT: SAFE state.\nSafe sequence: ");
        for (int k = 0; k < s.n; k++)
            printf("P%d%s", seq[k], k + 1 < s.n ? " -> " : "\n");
        printf("(Safe sequences are not unique; this is the one found by scanning P0..P%d in order.)\n",
               s.n - 1);
    } else {
        char w[160], nd[160];
        vec_str(work, s.m, w, sizeof w);
        printf("RESULT: UNSAFE state - no safe sequence exists.\n");
        printf("Processes that finished before the algorithm got stuck: ");
        if (done == 0) printf("none");
        for (int k = 0; k < done; k++)
            printf("P%d%s", seq[k], k + 1 < done ? ", " : "");
        printf("\nWork when stuck = %s. Processes that can never finish:\n", w);
        for (int i = 0; i < s.n; i++) {
            if (finish[i]) continue;
            vec_str(s.need[i], s.m, nd, sizeof nd);
            printf("  P%d: Need %s exceeds Work %s in resource(s):", i, nd, w);
            for (int j = 0; j < s.m; j++)
                if (s.need[i][j] > work[j])
                    printf(" %c", 'A' + j);
            printf("\n");
        }
    }
    return RC_OK;     /* the analysis itself succeeded, whatever the verdict */
}