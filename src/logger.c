/*
 * logger.c - concurrent activity logger (FR3, owner: Person B).
 *
 * Usage: logger [--items N] [--buffer N] [--producers N] [--log FILE]
 *   --items N      events produced by EACH producer thread   (default 5)
 *   --buffer N     capacity of the bounded queue             (default 4)
 *   --producers N  number of producer threads                (default 3)
 *   --log FILE     log file written by the consumer          (default campuscompute.log)
 *
 * Design: classic bounded buffer (circular array).
 *   - one mutex protects the queue and its counters
 *   - condition variable not_full  : producers wait here when the queue is full
 *   - condition variable not_empty : the consumer waits here when it is empty
 *   - shutdown: after all producers are joined, main sets 'done' and wakes the
 *     consumer, which first DRAINS the queue and then exits; main joins it.
 */
#include "common.h"

#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#define DEF_ITEMS      5
#define DEF_BUFFER     4
#define DEF_PRODUCERS  3
#define MAX_ITEMS      1000
#define MAX_BUFFER     64
#define MAX_PRODUCERS  8
#define MSG_LEN        64

typedef struct {
    int  seq;                     /* global sequence number (assigned in queue order) */
    int  producer;                /* producer id (1-based)                            */
    char msg[MSG_LEN];            /* event text                                       */
} Event;

typedef struct {
    Event          *buf;
    int             cap, head, tail, count;
    int             done;         /* set by main once all producers have finished     */
    int             next_seq;
    int             produced, consumed;
    int             full_waits;   /* how many times a producer had to wait (stats)    */
    int             empty_waits;  /* how many times the consumer had to wait (stats)  */
    pthread_mutex_t lock;
    pthread_cond_t  not_full, not_empty;
    FILE           *log;
} Queue;

typedef struct { Queue *q; int id; int items; } ProducerArg;

static const char *EVENTS[] = {
    "JOB_SUBMITTED", "JOB_STARTED", "RESOURCE_REQUESTED",
    "RESOURCE_GRANTED", "JOB_FINISHED"
};
#define N_EVENTS ((int)(sizeof(EVENTS) / sizeof(EVENTS[0])))

static void msleep(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void timestamp(char *out, size_t n)
{
    struct timespec now;
    struct tm tmv;
    clock_gettime(CLOCK_REALTIME, &now);
    localtime_r(&now.tv_sec, &tmv);
    size_t len = strftime(out, n, "%Y-%m-%d %H:%M:%S", &tmv);
    snprintf(out + len, n - len, ".%03ld", now.tv_nsec / 1000000L);
}

/* ---- producer thread ---------------------------------------------------- */
static void *producer_main(void *arg)
{
    ProducerArg *pa = arg;
    Queue *q = pa->q;
    unsigned seed = (unsigned)(time(NULL) ^ (pa->id * 7919));

    for (int i = 0; i < pa->items; i++) {
        msleep(10 + rand_r(&seed) % 40);               /* simulate doing some work */

        pthread_mutex_lock(&q->lock);
        if (q->count == q->cap) {
            q->full_waits++;
            printf("[producer %d] queue FULL (%d/%d) - waiting\n",
                   pa->id, q->count, q->cap);
            while (q->count == q->cap)
                pthread_cond_wait(&q->not_full, &q->lock);
        }
        Event *e = &q->buf[q->tail];
        e->seq = ++q->next_seq;
        e->producer = pa->id;
        snprintf(e->msg, MSG_LEN, "%s (item %d of %d)",
                 EVENTS[rand_r(&seed) % N_EVENTS], i + 1, pa->items);
        q->tail = (q->tail + 1) % q->cap;
        q->count++;
        q->produced++;
        printf("[producer %d] produced #%d: %s   (queue %d/%d)\n",
               pa->id, e->seq, e->msg, q->count, q->cap);
        pthread_cond_signal(&q->not_empty);
        pthread_mutex_unlock(&q->lock);
    }
    printf("[producer %d] finished\n", pa->id);
    return NULL;
}

/* ---- consumer thread ---------------------------------------------------- */
static void *consumer_main(void *arg)
{
    Queue *q = arg;

    for (;;) {
        pthread_mutex_lock(&q->lock);
        if (q->count == 0 && !q->done)
            q->empty_waits++;
        while (q->count == 0 && !q->done)
            pthread_cond_wait(&q->not_empty, &q->lock);
        if (q->count == 0 && q->done) {                /* drained and no more input */
            pthread_mutex_unlock(&q->lock);
            break;
        }
        Event e = q->buf[q->head];
        q->head = (q->head + 1) % q->cap;
        q->count--;
        q->consumed++;
        pthread_cond_signal(&q->not_full);
        pthread_mutex_unlock(&q->lock);

        /* file I/O happens OUTSIDE the lock so producers are not blocked by it */
        char ts[40];
        timestamp(ts, sizeof ts);
        fprintf(q->log, "[%s] #%03d producer=%d event=%s\n", ts, e.seq, e.producer, e.msg);
        fflush(q->log);
        printf("                         [consumer] logged #%d from producer %d\n",
               e.seq, e.producer);
        msleep(30);                                    /* simulate slow disk write */
    }
    printf("[consumer] queue drained, exiting\n");
    return NULL;
}

/* ---- argument parsing ---------------------------------------------------- */
static int parse_int(const char *s, int lo, int hi, int *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || end == s || *end != '\0' || v < lo || v > hi)
        return -1;
    *out = (int)v;
    return 0;
}

int run_logger(int argc, char **argv)
{
    int items = DEF_ITEMS, bufsz = DEF_BUFFER, nprod = DEF_PRODUCERS;
    const char *logpath = DEFAULT_LOG_FILE;

    for (int i = 0; i < argc; i++) {
        int rc = -1;
        if (i + 1 >= argc) {
            fprintf(stderr, "error: option '%s' needs a value (or is unknown)\n", argv[i]);
            return RC_USAGE;
        }
        if (strcmp(argv[i], "--items") == 0)          rc = parse_int(argv[++i], 1, MAX_ITEMS, &items);
        else if (strcmp(argv[i], "--buffer") == 0)    rc = parse_int(argv[++i], 1, MAX_BUFFER, &bufsz);
        else if (strcmp(argv[i], "--producers") == 0) rc = parse_int(argv[++i], 1, MAX_PRODUCERS, &nprod);
        else if (strcmp(argv[i], "--log") == 0)     { logpath = argv[++i]; rc = 0; }
        else {
            fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
            return RC_USAGE;
        }
        if (rc != 0) {
            fprintf(stderr, "error: invalid value '%s' for option '%s' "
                    "(items 1-%d, buffer 1-%d, producers 1-%d)\n",
                    argv[i], argv[i - 1], MAX_ITEMS, MAX_BUFFER, MAX_PRODUCERS);
            return RC_USAGE;
        }
    }

    Queue q;
    memset(&q, 0, sizeof q);
    q.cap = bufsz;
    q.buf = calloc((size_t)bufsz, sizeof(Event));
    q.log = fopen(logpath, "w");
    if (q.buf == NULL || q.log == NULL) {
        fprintf(stderr, "error: cannot %s: %s\n",
                q.buf ? "open log file" : "allocate queue", strerror(errno));
        if (q.log) fclose(q.log);
        free(q.buf);
        return RC_ERR;
    }
    pthread_mutex_init(&q.lock, NULL);
    pthread_cond_init(&q.not_full, NULL);
    pthread_cond_init(&q.not_empty, NULL);

    printf("Logger: %d producer(s) x %d event(s), queue capacity %d, 1 consumer\n",
           nprod, items, bufsz);
    printf("Log file: %s\n\n", logpath);

    int rc = RC_OK;
    pthread_t cons, prod[MAX_PRODUCERS];
    ProducerArg pargs[MAX_PRODUCERS];
    int started = 0;

    if (pthread_create(&cons, NULL, consumer_main, &q) != 0) {
        fprintf(stderr, "error: cannot create consumer thread\n");
        rc = RC_ERR;
    } else {
        for (int i = 0; i < nprod; i++) {
            pargs[i] = (ProducerArg){ &q, i + 1, items };
            if (pthread_create(&prod[i], NULL, producer_main, &pargs[i]) != 0) {
                fprintf(stderr, "error: cannot create producer thread %d\n", i + 1);
                rc = RC_ERR;
                break;
            }
            started++;
        }
        for (int i = 0; i < started; i++)              /* 1. wait for all producers */
            pthread_join(prod[i], NULL);

        pthread_mutex_lock(&q.lock);                   /* 2. tell consumer: no more input */
        q.done = 1;
        pthread_cond_broadcast(&q.not_empty);
        pthread_mutex_unlock(&q.lock);
        pthread_join(cons, NULL);                      /* 3. consumer drains, then exits */
    }

    int expected = started * items;
    printf("\n--- Summary ---\n");
    printf("Produced: %d   Logged: %d   Expected: %d\n", q.produced, q.consumed, expected);
    printf("Producer waits (queue full): %d   Consumer waits (queue empty): %d\n",
           q.full_waits, q.empty_waits);
    if (q.produced == q.consumed && q.produced == expected && rc == RC_OK)
        printf("All %d events logged; all %d threads joined cleanly.\n",
               q.consumed, started + 1);
    else {
        printf("ERROR: event count mismatch or thread failure.\n");
        rc = RC_ERR;
    }

    if (fclose(q.log) != 0) { perror("fclose"); rc = RC_ERR; }
    pthread_cond_destroy(&q.not_full);
    pthread_cond_destroy(&q.not_empty);
    pthread_mutex_destroy(&q.lock);
    free(q.buf);
    return rc;
}
