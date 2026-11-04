/*
 * ipc.c - POSIX shared memory + named semaphores (FR4, owner: Person B).
 *
 * Usage: ipc <run|cleanup>
 *   run     : parent and child (fork) exchange data through a shared-memory
 *             region. Two named semaphores coordinate them:
 *               /campuscompute_sem      "data ready"  (parent -> child)
 *               /campuscompute_sem_ack  "result ready" (child -> parent)
 *             The parent writes numbers, the child sums them and writes the
 *             result back. All objects are removed again before returning.
 *   cleanup : remove any leftover /campuscompute* objects (e.g. after a crash).
 *
 * Only objects whose name starts with IPC_PREFIX ("/campuscompute") are ever
 * unlinked, so other programs' IPC objects are never touched.
 */
#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SHM_NAME   IPC_PREFIX "_shm"
#define SEM_DATA   IPC_PREFIX "_sem"
#define SEM_ACK    IPC_PREFIX "_sem_ack"

#define ROUNDS        3
#define NVALUES       5
#define WAIT_SECONDS  5          /* never block forever if the peer dies */

typedef struct {
    int  round;
    int  quit;                   /* parent sets this to tell the child to stop */
    int  count;
    int  values[NVALUES];
    long result;
    char text[64];
} Shared;

/* sem_wait with timeout; returns 0 on success, -1 on timeout/error */
static int wait_sem(sem_t *s)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += WAIT_SECONDS;
    int r;
    do {
        r = sem_timedwait(s, &ts);
    } while (r < 0 && errno == EINTR);
    return r;
}

/* Safety check: refuse to unlink anything outside our prefix. */
static int our_name(const char *name)
{
    return strncmp(name, IPC_PREFIX, strlen(IPC_PREFIX)) == 0;
}

/* Unlink one object. kind: 's' = shared memory, 'm' = semaphore.
 * Returns 1 if removed, 0 if it did not exist, -1 on error. */
static int unlink_one(char kind, const char *name)
{
    if (!our_name(name)) {
        fprintf(stderr, "refusing to unlink '%s' (not a %s object)\n", name, IPC_PREFIX);
        return -1;
    }
    int r = (kind == 's') ? shm_unlink(name) : sem_unlink(name);
    if (r == 0)
        return 1;
    if (errno == ENOENT)
        return 0;
    fprintf(stderr, "unlink %s: %s\n", name, strerror(errno));
    return -1;
}

static int cleanup_all(int verbose)
{
    const struct { char kind; const char *name; } objs[] = {
        { 's', SHM_NAME }, { 'm', SEM_DATA }, { 'm', SEM_ACK }
    };
    int rc = RC_OK;
    for (size_t i = 0; i < sizeof objs / sizeof objs[0]; i++) {
        int r = unlink_one(objs[i].kind, objs[i].name);
        if (r < 0) rc = RC_ERR;
        if (verbose)
            printf("  %-28s %s\n", objs[i].name,
                   r == 1 ? "removed" : r == 0 ? "not found (already clean)" : "ERROR");
    }
    return rc;
}

/* Confirm the objects are really gone (open WITHOUT O_CREAT must fail). */
static int verify_gone(void)
{
    int ok = 1;
    int fd = shm_open(SHM_NAME, O_RDWR, 0);
    if (fd >= 0) { close(fd); ok = 0; }
    sem_t *a = sem_open(SEM_DATA, 0);
    if (a != SEM_FAILED) { sem_close(a); ok = 0; }
    sem_t *b = sem_open(SEM_ACK, 0);
    if (b != SEM_FAILED) { sem_close(b); ok = 0; }
    return ok;
}

/* ---- child: consumer of shared data -------------------------------------- */
static void child_main(Shared *sh, sem_t *data, sem_t *ack)
{
    printf("[child  pid=%d] ready, waiting for data on semaphore\n", (int)getpid());
    fflush(stdout);
    for (;;) {
        if (wait_sem(data) != 0) {
            fprintf(stderr, "[child] timed out waiting for parent\n");
            break;
        }
        if (sh->quit) {
            printf("[child  pid=%d] quit flag seen, exiting\n", (int)getpid());
            fflush(stdout);
            sem_close(data); sem_close(ack); munmap(sh, sizeof *sh);
            _exit(RC_OK);
        }
        long sum = 0;
        printf("[child  pid=%d] round %d: read \"%s\" values:", (int)getpid(), sh->round, sh->text);
        for (int i = 0; i < sh->count; i++) {
            printf(" %d", sh->values[i]);
            sum += sh->values[i];
        }
        printf("  -> sum = %ld\n", sum);
        fflush(stdout);
        sh->result = sum;
        sem_post(ack);
    }
    sem_close(data); sem_close(ack); munmap(sh, sizeof *sh);
    _exit(RC_ERR);
}

static int do_run(void)
{
    int rc = RC_ERR, fd = -1, created_shm = 0, created_sem = 0;
    Shared *sh = MAP_FAILED;
    sem_t *data = SEM_FAILED, *ack = SEM_FAILED;
    pid_t pid = -1;

    printf("Step 1: remove stale objects (if any)\n");
    cleanup_all(1);

    printf("\nStep 2: create shared memory and semaphores\n");
    fd = shm_open(SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) { perror("shm_open"); goto out; }
    created_shm = 1;
    if (ftruncate(fd, sizeof(Shared)) != 0) { perror("ftruncate"); goto out; }
    sh = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (sh == MAP_FAILED) { perror("mmap"); goto out; }
    memset(sh, 0, sizeof *sh);

    data = sem_open(SEM_DATA, O_CREAT | O_EXCL, 0600, 0);
    ack  = sem_open(SEM_ACK,  O_CREAT | O_EXCL, 0600, 0);
    if (data == SEM_FAILED || ack == SEM_FAILED) { perror("sem_open"); goto out; }
    created_sem = 1;
    printf("  created %s (%zu bytes), %s, %s\n", SHM_NAME, sizeof(Shared), SEM_DATA, SEM_ACK);

    printf("\nStep 3: fork child and exchange data\n");
    fflush(stdout);
    pid = fork();
    if (pid < 0) { perror("fork"); goto out; }
    if (pid == 0)
        child_main(sh, data, ack);

    int ok = 1;
    for (int r = 1; r <= ROUNDS && ok; r++) {
        sh->round = r;
        sh->count = NVALUES;
        long expect = 0;
        for (int i = 0; i < NVALUES; i++) {
            sh->values[i] = r * (i + 1);
            expect += sh->values[i];
        }
        snprintf(sh->text, sizeof sh->text, "message %d from parent", r);
        printf("[parent pid=%d] round %d: wrote \"%s\" to shared memory, posting semaphore\n",
               (int)getpid(), r, sh->text);
        fflush(stdout);
        sem_post(data);
        if (wait_sem(ack) != 0) {
            fprintf(stderr, "[parent] timed out waiting for child's result\n");
            ok = 0;
            break;
        }
        printf("[parent pid=%d] round %d: child's result = %ld (expected %ld) %s\n",
               (int)getpid(), r, sh->result, expect, sh->result == expect ? "OK" : "MISMATCH");
        if (sh->result != expect) ok = 0;
    }
    if (ok) {
        sh->quit = 1;
        sem_post(data);
    } else {
        kill(pid, SIGKILL);
    }

    int status = 0;
    pid_t w;
    do { w = waitpid(pid, &status, 0); } while (w < 0 && errno == EINTR);
    pid = -1;
    if (w < 0) { perror("waitpid"); goto out; }
    if (ok && WIFEXITED(status) && WEXITSTATUS(status) == RC_OK)
        rc = RC_OK;
    else
        fprintf(stderr, "[parent] child did not finish cleanly\n");

out:
    if (pid > 0) { kill(pid, SIGKILL); waitpid(pid, NULL, 0); }
    if (sh != MAP_FAILED) munmap(sh, sizeof *sh);
    if (fd >= 0) close(fd);
    if (data != SEM_FAILED) sem_close(data);
    if (ack != SEM_FAILED) sem_close(ack);

    printf("\nStep 4: cleanup\n");
    (void)created_shm; (void)created_sem;
    if (cleanup_all(1) != RC_OK) rc = RC_ERR;
    if (verify_gone())
        printf("  verified: no %s objects remain\n", IPC_PREFIX);
    else {
        printf("  ERROR: some objects still exist\n");
        rc = RC_ERR;
    }
    return rc;
}

int run_ipc(int argc, char **argv)
{
    if (argc != 1)
        return RC_USAGE;
    if (strcmp(argv[0], "run") == 0)
        return do_run();
    if (strcmp(argv[0], "cleanup") == 0) {
        printf("Removing leftover %s* objects:\n", IPC_PREFIX);
        int rc = cleanup_all(1);
        if (verify_gone()) printf("verified: no %s objects remain\n", IPC_PREFIX);
        return rc;
    }
    fprintf(stderr, "error: unknown ipc action '%s'\n", argv[0]);
    return RC_USAGE;
}
