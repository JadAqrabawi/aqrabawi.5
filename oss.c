// oss.c
// Master simulator: resource management with deadlock detection & recovery

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/wait.h>
#include <unistd.h>      // for fork(), getopt(), optarg, usleep()
#include <getopt.h>  

#define RESOURCE_COUNT          5
#define INSTANCES_PER_RESOURCE 10
#define MAX_ACTIVE_PROCS       18

// default tuning parameters
#define CLOCK_INCREMENT_NS 100000000  // 0.1s per loop
#define PRINT_TABLE_NS     500000000  // 0.5s
#define DEADLOCK_NS       1000000000  // 1.0s
#define REAL_TIME_LIMIT      5        // seconds

// message types
#define REQ_TYPE  1
#define RES_TYPE(pid) (pid)

// request kinds
#define REQUEST     0
#define RELEASE_ONE 1
#define RELEASE_ALL 2

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

typedef struct {
    int total;
    int available;
} resource_t;

typedef struct {
    pid_t pid;
    int in_use;
    int allocated[RESOURCE_COUNT];
    int waiting;
    int req_res;
} pcb_t;

typedef struct {
    int total_reqs;
    int immediate_grants;
    int delayed_reqs;
    int deadlock_runs;
    int procs_killed;
    int procs_terminated_normally;
} stats_t;

static int shm_id = -1, msg_id = -1;
static shm_clock_t *clk = NULL;
static resource_t resources[RESOURCE_COUNT];
static pcb_t proc_tbl[MAX_ACTIVE_PROCS];
static stats_t stats;
static FILE *logfp = NULL;

// helper: log to stdout and logfile
void log_event(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    fprintf(stdout, "\n");
    if (logfp) {
        va_start(ap, fmt);
        vfprintf(logfp, fmt, ap);
        fprintf(logfp, "\n");
    }
    va_end(ap);
}

// increment shared clock by ns, rolling seconds
void increment_clock(unsigned int ns) {
    clk->ns += ns;
    while (clk->ns >= 1000000000U) {
        clk->sec++;
        clk->ns -= 1000000000U;
    }
}

// print resource & process table
void print_tables() {
    log_event("---- Resource Table at %u:%09u ----", clk->sec, clk->ns);
    for (int i = 0; i < RESOURCE_COUNT; i++) {
        log_event("R%d: %d available / %d total",
                  i, resources[i].available, resources[i].total);
    }
    log_event("---- Process Table ----");
    for (int i = 0; i < MAX_ACTIVE_PROCS; i++) {
        if (!proc_tbl[i].in_use) continue;
        printf("PID %d:", proc_tbl[i].pid);
        if (logfp) fprintf(logfp, "PID %d:", proc_tbl[i].pid);
        for (int r = 0; r < RESOURCE_COUNT; r++) {
            printf(" R%d=%d", r, proc_tbl[i].allocated[r]);
            if (logfp) fprintf(logfp, " R%d=%d", r, proc_tbl[i].allocated[r]);
        }
        printf("\n");
        if (logfp) fprintf(logfp, "\n");
    }
}

// find pcb entry by pid
pcb_t* find_pcb(pid_t pid) {
    for (int i = 0; i < MAX_ACTIVE_PROCS; i++)
        if (proc_tbl[i].in_use && proc_tbl[i].pid == pid)
            return &proc_tbl[i];
    return NULL;
}

// try granting outstanding requests
void check_waiting() {
    for (int i = 0; i < MAX_ACTIVE_PROCS; i++) {
        if (!proc_tbl[i].in_use || !proc_tbl[i].waiting) continue;
        int rid = proc_tbl[i].req_res;
        if (resources[rid].available > 0) {
            // allocate
            resources[rid].available--;
            proc_tbl[i].allocated[rid]++;
            proc_tbl[i].waiting = 0;
            stats.immediate_grants++;  // count as grant after wait
            // send grant
            grant_t g = { .mtype = proc_tbl[i].pid, .resource_id = rid };
            msgsnd(msg_id, &g, sizeof(g) - sizeof(long), 0);
            log_event("Master granting delayed request PID %d R%d at %u:%09u",
                      proc_tbl[i].pid, rid, clk->sec, clk->ns);
        }
    }
}

// detect deadlock via simple wait-for graph cycle detection
// returns number of pids found, array filled with them
int detect_deadlock(pid_t out[]) {
    int n = 0;
    // build wait-for adjacency matrix
    int waiting[MAX_ACTIVE_PROCS] = {0};
    int held_by[RESOURCE_COUNT][MAX_ACTIVE_PROCS] = {{0}};
    int proc_index[MAX_ACTIVE_PROCS];
    // map active entries to index
    int m = 0;
    for (int i = 0; i < MAX_ACTIVE_PROCS; i++) {
        if (proc_tbl[i].in_use) proc_index[m++] = i;
    }
    // who holds each resource?
    for (int k = 0; k < m; k++) {
        int i = proc_index[k];
        for (int r = 0; r < RESOURCE_COUNT; r++) {
            if (proc_tbl[i].allocated[r] > 0)
                held_by[r][k] = 1;
        }
        if (proc_tbl[i].waiting) waiting[k] = 1;
    }
    // build wait-for edges
    int adj[m][m];
    memset(adj, 0, sizeof(adj));
    for (int k = 0; k < m; k++) {
        if (!waiting[k]) continue;
        int rid = proc_tbl[proc_index[k]].req_res;
        for (int j = 0; j < m; j++) {
            if (held_by[rid][j]) adj[k][j] = 1;
        }
    }
    // detect cycle with DFS
    int visited[m], stack[m];
    memset(visited, 0, sizeof(visited));
    memset(stack,   0, sizeof(stack));
    // lambda DFS
    int found = 0;
    void dfs(int u) {
        if (found) return;
        visited[u] = 1; stack[u] = 1;
        for (int v = 0; v < m; v++) {
            if (!adj[u][v]) continue;
            if (!visited[v]) dfs(v);
            else if (stack[v]) {
                // cycle: collect all in stack
                for (int x = 0; x < m; x++) {
                    if (stack[x]) {
                        out[n++] = proc_tbl[proc_index[x]].pid;
                    }
                }
                found = 1;
                return;
            }
        }
        stack[u] = 0;
    }
    for (int k = 0; k < m && !found; k++)
        if (!visited[k] && waiting[k])
            dfs(k);

    return n;
}

// choose victim: highest pid
pid_t choose_victim(pid_t pids[], int count) {
    pid_t best = pids[0];
    for (int i = 1; i < count; i++)
        if (pids[i] > best) best = pids[i];
    return best;
}

// free all resources of pcb, mark removed
void cleanup_pcb(pid_t pid, int killed) {
    pcb_t *p = find_pcb(pid);
    if (!p) return;
    // release allocs
    int freed_any = 0;
    for (int r = 0; r < RESOURCE_COUNT; r++) {
        if (p->allocated[r] > 0) {
            resources[r].available += p->allocated[r];
            freed_any = 1;
        }
    }
    if (freed_any) {
        log_event("Master freed resources of PID %d at %u:%09u",
                  pid, clk->sec, clk->ns);
    }
    if (killed) stats.procs_killed++;
    else         stats.procs_terminated_normally++;
    p->in_use = 0;
}

// clean up shared IPC
void cleanup_ipc(int sig) {
    (void)sig;  // Mark the parameter as intentionally unused
    if (clk) shmdt(clk);
    if (shm_id >= 0) shmctl(shm_id, IPC_RMID, NULL);
    if (msg_id >= 0) msgctl(msg_id, IPC_RMID, NULL);
    if (logfp) fclose(logfp);
    exit(0);
}


int main(int argc, char *argv[]) {
    int opt;
    int n_max = 5;
    unsigned int sim_limit = 20;      // simulated seconds
    unsigned int launch_i_ms = 250;   // ms
    unsigned int bound_ns = 100000000; // worker B
    char *logfile = NULL;

    while ((opt = getopt(argc, argv, "hn:s:i:b:f:")) != -1) {
        switch(opt) {
        case 'h': printf("Usage: oss [-n maxproc] [-s simsec] [-i launch_ms] "
                         "[-b bound_ns] [-f logfile]\n"); return 0;
        case 'n': n_max = atoi(optarg); break;
        case 's': sim_limit = atoi(optarg); break;
        case 'i': launch_i_ms = atoi(optarg); break;
        case 'b': bound_ns = atoi(optarg); break;
        case 'f': logfile = optarg; break;
        default:  fprintf(stderr,"Unknown option\n"); exit(1);
        }
    }
    if (!logfile) {
        fprintf(stderr, "Must specify -f logfile\n"); exit(1);
    }
    logfp = fopen(logfile, "w");
    if (!logfp) { perror("fopen"); exit(1); }

    signal(SIGINT, cleanup_ipc);
    signal(SIGTERM, cleanup_ipc);

    // setup shared clock
    key_t shm_key = ftok(".", 'c');
    shm_id = shmget(shm_key, sizeof(shm_clock_t), IPC_CREAT|0666);
    clk = shmat(shm_id, NULL, 0);
    clk->sec = clk->ns = 0;

    // setup message queue
    key_t msg_key = ftok(".", 'q');
    msg_id = msgget(msg_key, IPC_CREAT|0666);

    // init resources & proc table
    for (int i = 0; i < RESOURCE_COUNT; i++) {
        resources[i].total = INSTANCES_PER_RESOURCE;
        resources[i].available = INSTANCES_PER_RESOURCE;
    }
    memset(&proc_tbl, 0, sizeof(proc_tbl));
    memset(&stats,    0, sizeof(stats));

    unsigned int launch_interval_ns = launch_i_ms * 1000000U;
    unsigned int next_launch_time = 0;
    unsigned int last_print = 0, last_deadlock = 0;
    int total_launched = 0, active = 0;
    time_t real_start = time(NULL);
    int allow_launch = 1;

    // main loop
    while ((total_launched < n_max && allow_launch) || active > 0) {
        // real-time stop launching
        if (difftime(time(NULL), real_start) > REAL_TIME_LIMIT)
            allow_launch = 0;

        increment_clock(CLOCK_INCREMENT_NS);

        // reap any terminated child
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid > 0) {
            cleanup_pcb(pid, WIFSIGNALED(status));
            active--;
            check_waiting();
        }

        // possibly launch new worker
        unsigned int cur_ns = clk->sec * 1000000000U + clk->ns;
        if (allow_launch && total_launched < n_max && active < MAX_ACTIVE_PROCS
         && cur_ns >= next_launch_time) {
            pid_t c = fork();
            if (c < 0) { perror("fork"); }
            else if (c == 0) {
                // child → exec worker
                char bn[32];
                snprintf(bn, sizeof(bn), "%u", bound_ns);
                execl("./worker", "worker", "-b", bn, NULL);
                perror("execl"); _exit(1);
            } else {
                // parent
                // find free slot
                for (int i = 0; i < MAX_ACTIVE_PROCS; i++) {
                    if (!proc_tbl[i].in_use) {
                        proc_tbl[i].in_use = 1;
                        proc_tbl[i].pid    = c;
                        for (int r = 0; r < RESOURCE_COUNT; r++)
                            proc_tbl[i].allocated[r] = 0;
                        proc_tbl[i].waiting = 0;
                        break;
                    }
                }
                total_launched++; active++;
                next_launch_time = cur_ns + launch_interval_ns;
                log_event("Master launched PID %d at %u:%09u",
                          c, clk->sec, clk->ns);
            }
        }

        // grant any waiting if possible
        check_waiting();

        // handle incoming requests/releases
        msg_t m;
        if (msgrcv(msg_id, &m, sizeof(m)-sizeof(long), REQ_TYPE,
                   IPC_NOWAIT) > 0) {
            stats.total_reqs++;
            pcb_t *p = find_pcb(m.pid);
            if (!p) continue;
            switch (m.kind) {
            case REQUEST:
                log_event("Master detected PID %d REQUEST R%d at %u:%09u",
                          m.pid, m.resource_id, clk->sec, clk->ns);
                if (resources[m.resource_id].available > 0) {
                    // grant immediately
                    resources[m.resource_id].available--;
                    p->allocated[m.resource_id]++;
                    stats.immediate_grants++;
                    grant_t g = { .mtype = m.pid,
                                  .resource_id = m.resource_id };
                    msgsnd(msg_id, &g, sizeof(g)-sizeof(long), 0);
                    log_event("Master granting PID %d R%d at %u:%09u",
                              m.pid, m.resource_id, clk->sec, clk->ns);
                } else {
                    // block
                    p->waiting    = 1;
                    p->req_res    = m.resource_id;
                    stats.delayed_reqs++;
                    log_event("Master blocking PID %d on R%d at %u:%09u",
                              m.pid, m.resource_id, clk->sec, clk->ns);
                }
                break;
            case RELEASE_ONE:
                resources[m.resource_id].available++;
                p->allocated[m.resource_id]--;
                log_event("Master detected PID %d RELEASE R%d at %u:%09u",
                          m.pid, m.resource_id, clk->sec, clk->ns);
                check_waiting();
                break;
            case RELEASE_ALL:
                log_event("Master detected PID %d RELEASING ALL at %u:%09u",
                          m.pid, clk->sec, clk->ns);
                cleanup_pcb(m.pid, 0);
                active--;
                check_waiting();
                break;
            }
        }

        // periodic table print
        cur_ns = clk->sec * 1000000000U + clk->ns;
        if (cur_ns - last_print >= PRINT_TABLE_NS) {
            print_tables();
            last_print = cur_ns;
        }

        // periodic deadlock detect
        if (cur_ns - last_deadlock >= DEADLOCK_NS) {
            stats.deadlock_runs++;
            pid_t dead[ MAX_ACTIVE_PROCS ];
            int dn = detect_deadlock(dead);
            if (dn > 0) {
                log_event("Master running deadlock detection at %u:%09u: found %d",
                          clk->sec, clk->ns, dn);
                // kill one victim at a time until resolved
                while (dn > 0) {
                    pid_t victim = choose_victim(dead, dn);
                    log_event("Master terminating PID %d to resolve deadlock", victim);
                    kill(victim, SIGTERM);
                    // wait for reap in next iteration
                    sleep(0);  
                    dn = detect_deadlock(dead);
                }
            }
            last_deadlock = cur_ns;
        }
        // busy‐wait a tiny bit
        usleep(1000);
    }

    // final stats
    log_event("=== Simulation complete at %u:%09u ===", clk->sec, clk->ns);
    log_event("Total requests: %d", stats.total_reqs);
    log_event("Immediate grants: %d", stats.immediate_grants);
    log_event("Delayed grants: %d", stats.delayed_reqs);
    log_event("Deadlock checks: %d", stats.deadlock_runs);
    log_event("Processes killed by deadlock: %d", stats.procs_killed);
    log_event("Processes terminated normally: %d",
              stats.procs_terminated_normally);

    cleanup_ipc(0);
    return 0;
}
