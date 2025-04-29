// worker.c
// User process: randomly requests/releases and terminates after some time.

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <unistd.h>      // for fork(), getopt(), optarg, usleep()
#include <getopt.h>  

#define RESOURCE_COUNT 5

#define REQ_TYPE 1
#define RES_TYPE(pid) (pid)

#define REQUEST     0
#define RELEASE_ONE 1
#define RELEASE_ALL 2

// read‐bound for request/release generation
static unsigned int BOUND_NS = 100000000;

// how often to check termination (250ms)
#define TERM_CHECK_NS 250000000
// minimum run time before eligible to terminate (1s)
#define MIN_RUN_NS   1000000000

// probability percentages
#define REQUEST_PROB     80  // % chance to request vs release
#define TERMINATE_PROB   10  // % chance to terminate at each check

typedef struct {
    long mtype;
    pid_t pid;
    int kind;
    int resource_id;
} msg_t;

typedef struct {
    long mtype;
    int resource_id;
} grant_t;

typedef struct {
    unsigned int sec;
    unsigned int ns;
} shm_clock_t;

int main(int argc, char *argv[]) {
    int opt;
    while ((opt = getopt(argc, argv, "b:")) != -1) {
        if (opt=='b') BOUND_NS = atoi(optarg);
    }

    // attach to shared clock
    key_t shm_key = ftok(".", 'c');
    int shm_id = shmget(shm_key, sizeof(shm_clock_t), 0666);
    shm_clock_t *clk = shmat(shm_id, NULL, 0);

    // attach to message queue
    key_t msg_key = ftok(".", 'q');
    int msg_id = msgget(msg_key, 0666);

    // random seed
    srand(getpid() ^ time(NULL));

    // track local allocations
    int alloc[RESOURCE_COUNT] = {0};

    // timing
    unsigned long start_ns = clk->sec * 1000000000U + clk->ns;
    unsigned long last_action_ns = start_ns;
    unsigned long next_action_delay = rand() % (BOUND_NS+1);
    unsigned long last_term_ns = start_ns;

    while (1) {
        unsigned long now = clk->sec * 1000000000U + clk->ns;

        // check termination eligibility
        if (now - start_ns >= MIN_RUN_NS
         && now - last_term_ns >= TERM_CHECK_NS) {
            if ((rand() % 100) < TERMINATE_PROB) {
                // release all and exit
                msg_t m = { .mtype = REQ_TYPE, .pid = getpid(),
                            .kind  = RELEASE_ALL, .resource_id = -1 };
                msgsnd(msg_id, &m, sizeof(m)-sizeof(long), 0);
                shmdt(clk);
                exit(0);
            }
            last_term_ns = now;
        }

        // check if time to act
        if (now - last_action_ns >= next_action_delay) {
            // decide request vs release
            int do_req = (rand()%100) < REQUEST_PROB;
            // if no alloc, must request
            int can_release = 0;
            for (int r=0; r<RESOURCE_COUNT; r++)
                if (alloc[r]>0) { can_release=1; break; }
            if (!can_release) do_req = 1;

            if (do_req) {
                // pick random resource
                int rid = rand() % RESOURCE_COUNT;
                // only if not at system max
                msg_t m = { .mtype = REQ_TYPE, .pid = getpid(),
                            .kind  = REQUEST, .resource_id = rid };
                msgsnd(msg_id, &m, sizeof(m)-sizeof(long), 0);
                // wait for grant
                grant_t g;
                msgrcv(msg_id, &g, sizeof(g)-sizeof(long),
                       RES_TYPE(getpid()), 0);
                alloc[g.resource_id]++;
            } else {
                // release one from those held
                int choices[RESOURCE_COUNT], nc=0;
                for (int r=0; r<RESOURCE_COUNT; r++)
                    if (alloc[r]>0) choices[nc++]=r;
                int rid = choices[rand()%nc];
                msg_t m = { .mtype = REQ_TYPE, .pid = getpid(),
                            .kind  = RELEASE_ONE,
                            .resource_id = rid };
                msgsnd(msg_id, &m, sizeof(m)-sizeof(long), 0);
                alloc[rid]--;
            }

            // schedule next
            last_action_ns = now;
            next_action_delay = rand() % (BOUND_NS+1);
        }
        // small sleep to avoid busy loop
        usleep(1000);
    }
    return 0;
}