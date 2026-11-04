/*
 * jobs.c - job representation, input parsing and validation (FR1).
 *
 * Job file format: one job per line -> "id arrival burst priority"
 * Blank lines and lines starting with '#' are ignored; '#' after the numbers starts a comment.
 */
#include "common.h"
#include <ctype.h>
#include <errno.h>

#define LINE_MAX_LEN 256
#define FIELD_LIMIT  1000000L   /* reject absurd values early */

const char *job_status_str(JobStatus s)
{
    switch (s) {
    case JOB_NEW:     return "NEW";
    case JOB_READY:   return "READY";
    case JOB_RUNNING: return "RUNNING";
    case JOB_DONE:    return "DONE";
    }
    return "?";
}

/* Put every job back to its initial state so a scheduler can be run again on the same data. */
void jobs_reset_state(Job *jobs, int n)
{
    for (int i = 0; i < n; i++) {
        jobs[i].remaining  = jobs[i].burst;
        jobs[i].start      = -1;
        jobs[i].completion = -1;
        jobs[i].turnaround = 0;
        jobs[i].waiting    = 0;
        jobs[i].status     = JOB_NEW;
    }
}

void print_job_table(const Job *jobs, int n)
{
    printf("%-6s %-8s %-6s %-9s %-8s\n", "JobID", "Arrival", "Burst", "Priority", "Status");
    for (int i = 0; i < n; i++)
        printf("J%-5d %-8d %-6d %-9d %-8s\n", jobs[i].id, jobs[i].arrival, jobs[i].burst,
               jobs[i].priority, job_status_str(jobs[i].status));
}

/* Parse and validate the job file. Fills jobs[0..*n-1]. Returns RC_OK or RC_ERR. */
int load_jobs(const char *path, Job *jobs, int *n)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "error: cannot open job file '%s': %s\n", path, strerror(errno));
        return RC_ERR;
    }

    char line[LINE_MAX_LEN];
    int lineno = 0, count = 0;

    while (fgets(line, sizeof line, fp)) {
        lineno++;

        /* a line without '\n' that is not the last line of the file was truncated: too long */
        if (!strchr(line, '\n') && !feof(fp)) {
            fprintf(stderr, "error: %s:%d: line too long\n", path, lineno);
            fclose(fp);
            return RC_ERR;
        }

        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '\0' || *p == '#') continue;          /* blank line or comment */

        long v[4];
        int consumed = 0;
        if (sscanf(p, "%ld %ld %ld %ld %n", &v[0], &v[1], &v[2], &v[3], &consumed) != 4) {
            fprintf(stderr, "error: %s:%d: expected 4 integers 'id arrival burst priority'\n",
                    path, lineno);
            fclose(fp);
            return RC_ERR;
        }
        if (p[consumed] != '\0' && p[consumed] != '#') {
            fprintf(stderr, "error: %s:%d: unexpected extra text after the 4 fields\n", path, lineno);
            fclose(fp);
            return RC_ERR;
        }
        for (int k = 0; k < 4; k++) {
            if (v[k] < 0 || v[k] > FIELD_LIMIT) {
                fprintf(stderr, "error: %s:%d: values must be between 0 and %ld\n",
                        path, lineno, FIELD_LIMIT);
                fclose(fp);
                return RC_ERR;
            }
        }
        if (v[2] == 0) {
            fprintf(stderr, "error: %s:%d: CPU burst must be greater than 0\n", path, lineno);
            fclose(fp);
            return RC_ERR;
        }
        if (count >= MAX_JOBS) {
            fprintf(stderr, "error: %s: more than %d jobs\n", path, MAX_JOBS);
            fclose(fp);
            return RC_ERR;
        }
        for (int i = 0; i < count; i++) {
            if (jobs[i].id == (int)v[0]) {
                fprintf(stderr, "error: %s:%d: duplicate job ID %ld\n", path, lineno, v[0]);
                fclose(fp);
                return RC_ERR;
            }
        }

        jobs[count].id       = (int)v[0];
        jobs[count].arrival  = (int)v[1];
        jobs[count].burst    = (int)v[2];
        jobs[count].priority = (int)v[3];
        count++;
    }
    fclose(fp);

    if (count == 0) {
        fprintf(stderr, "error: '%s' contains no jobs\n", path);
        return RC_ERR;
    }

    *n = count;
    jobs_reset_state(jobs, count);
    return RC_OK;
}
