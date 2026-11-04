/*
 * common.h - shared definitions for CampusCompute.
 * Both teammates code against this header. Change it only after telling each other.
 */
#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_JOBS            32
#define RR_QUANTUM_DEFAULT  4
#define IPC_PREFIX          "/campuscompute"   /* shm: /campuscompute_shm, sem: /campuscompute_sem */
#define DEFAULT_LOG_FILE    "campuscompute.log"

/* Exit / return codes used by every run_* function and by main() */
#define RC_OK    0   /* success */
#define RC_ERR   1   /* runtime error or bad input data */
#define RC_USAGE 2   /* wrong command or wrong arguments */

/* Job lifecycle (FR1): NEW -> READY -> RUNNING -> DONE */
typedef enum { JOB_NEW, JOB_READY, JOB_RUNNING, JOB_DONE } JobStatus;

typedef struct {
    int id, arrival, burst, priority;   /* inputs; LOWER priority number = HIGHER priority */
    int remaining, start, completion;   /* filled in by the schedulers */
    int turnaround, waiting;            /* TAT = CT - AT, WT = TAT - BT */
    JobStatus status;
} Job;

/* ---- jobs.c (Person A) ---- */
int         load_jobs(const char *path, Job *jobs, int *n);  /* parse + validate; returns RC_* */
void        jobs_reset_state(Job *jobs, int n);              /* remaining=burst, status=NEW, ... */
const char *job_status_str(JobStatus s);
void        print_job_table(const Job *jobs, int n);

/* ---- main.c (Person A) ---- */
void print_usage(void);                  /* prints the command list to stdout */

/* ---- module entry points: argc/argv start AFTER the command word ---- */
int run_sched (int argc, char **argv);   /* Person A */
int run_banker(int argc, char **argv);   /* Person A */
int run_logger(int argc, char **argv);   /* Person B */
int run_ipc   (int argc, char **argv);   /* Person B */
int run_worker(int argc, char **argv);   /* Person B */
int run_limits(int argc, char **argv);   /* Person B */
int run_demo  (int argc, char **argv);   /* Person B: calls the other run_* in order */

#endif /* COMMON_H */
