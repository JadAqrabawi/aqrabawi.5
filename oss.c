/*
 * oss.c - Master process for OS Simulator
 * Manages child worker processes, resource allocation, and deadlock detection/recovery.
 */

 #include <stdio.h>
 #include <stdlib.h>
 #include <unistd.h>
 #include <sys/types.h>
 #include <sys/ipc.h>
 #include <sys/shm.h>
 #include <sys/msg.h>
 #include <sys/wait.h>
 #include <signal.h>
 #include <string.h>
 #include <errno.h>
 #include <time.h>
 #include <stdbool.h>
 
 #define SHMKEY 0x98765        // Shared memory key
 #define MSGKEY 0x12345        // Message queue key
 
 #define MAX_PROCESSES 100
 #define RES_TYPES 5
 #define INSTANCES_PER_RES 10
 
 // Message structure for communication between OSS and workers
 typedef struct {
     long mtype;             // Message type (for msgrcv/msqsnd)
     int action;             // Action code (request, release, etc.)
     int resource;           // Resource ID (0-4)
     int quantity;           // Quantity of resource
     unsigned int time;      // Simulated time used (nanoseconds)
     pid_t pid;              // Sender PID (for identification)
 } Message;
 
 // Action codes for messages (worker -> OSS)
 #define ACTION_REQUEST   1
 #define ACTION_RELEASE   2
 #define ACTION_TERMINATE 3
 #define ACTION_NOOP      4
 // Action codes for messages (OSS -> worker)
 #define ACTION_DISPATCH  1
 #define ACTION_GRANT     2
 #define ACTION_KILL      3
 
 // Shared memory and message queue identifiers
 int shmid = -1;
 int msqid = -1;
 unsigned int *shmClock = NULL;  // [0] = seconds, [1] = nanoseconds
 FILE *logFile = NULL;
 bool verbose = false;
 
 // Simulation parameters
 int totalProcesses = 0;
 int maxConcurrent = 0;
 unsigned int quantum_ns = 0;   // Time slice bound B in nanoseconds
 
 // Process control structure
 typedef struct {
     bool active;
     bool blocked;
     pid_t pid;
     unsigned int alloc[RES_TYPES];  // Resources allocated to this process
     int requestRes;    // Resource requested if blocked (-1 if none)
     int requestAmt;
 } ProcInfo;
 
 ProcInfo procTable[MAX_PROCESSES];
 int activeCount = 0;
 int launchedCount = 0;
 int terminatedCount = 0;
 
 // Resource availability (instances currently available for each of 5 resources)
 unsigned int available[RES_TYPES];
 
 // Statistics counters
 unsigned long totalRequests = 0;
 unsigned long totalReleases = 0;
 unsigned long requestsGrantedImmediately = 0;
 unsigned long requestsBlocked = 0;
 unsigned long deadlockDetections = 0;
 unsigned long deadlocksResolved = 0;
 unsigned long processesKilledForDeadlock = 0;
 
 // Cleanup function to release IPC resources and kill children
 void cleanup(int sig) {
     (void)sig;
     // Terminate all active children
     for(int i = 0; i < MAX_PROCESSES; ++i) {
         if(procTable[i].active) {
             kill(procTable[i].pid, SIGTERM);
         }
     }
     // Remove message queue
     if(msqid != -1) msgctl(msqid, IPC_RMID, NULL);
     // Detach and remove shared memory
     if(shmClock != NULL && shmClock != (void*)-1) shmdt(shmClock);
     if(shmid != -1) shmctl(shmid, IPC_RMID, NULL);
     if(logFile) fclose(logFile);
     exit(0);
 }
 
 /* Deadlock detection algorithm (runs every simulated second):
  * Marks non-blocked processes as safe, then checks which blocked processes can proceed.
  * Identifies processes in deadlock and terminates one holding the most resources to break the deadlock.
  */
 void detectDeadlock() {
     bool finish[MAX_PROCESSES];
     unsigned int work[RES_TYPES];
     // Initialize work as current available resources
     for(int r = 0; r < RES_TYPES; ++r) {
         work[r] = available[r];
     }
     // Initialize finish for each process
     for(int i = 0; i < MAX_PROCESSES; ++i) {
         if(procTable[i].active) {
             if(procTable[i].blocked) {
                 finish[i] = false;  // blocked processes not finished
             } else {
                 finish[i] = true;   // not blocked -> can finish
                 // Assume it will complete and free its resources
                 for(int r = 0; r < RES_TYPES; ++r) {
                     work[r] += procTable[i].alloc[r];
                 }
             }
         } else {
             finish[i] = true; // not active
         }
     }
     // Try to find processes that can finish (even if initially blocked)
     bool progress = true;
     while(progress) {
         progress = false;
         for(int i = 0; i < MAX_PROCESSES; ++i) {
             if(procTable[i].active && !finish[i]) {
                 int res = procTable[i].requestRes;
                 int amt = procTable[i].requestAmt;
                 // If this blocked process's request can be satisfied with current work
                 if(res >= 0 && (unsigned int)amt <= work[res]) {
                     finish[i] = true;
                     progress = true;
                     // Release its allocated resources into work
                     for(int r = 0; r < RES_TYPES; ++r) {
                         work[r] += procTable[i].alloc[r];
                     }
                 }
             }
         }
     }
     // Identify deadlocked processes (those still not finished)
     int deadlockedProcs[MAX_PROCESSES];
     int deadCount = 0;
     for(int i = 0; i < MAX_PROCESSES; ++i) {
         if(procTable[i].active && !finish[i]) {
             deadlockedProcs[deadCount++] = i;
         }
     }
     if(deadCount > 0) {
         deadlockDetections++;
         if(verbose) {
             fprintf(logFile ? logFile : stdout,
                     "OSS: Deadlock detected at time %u:%u involving %d process(es).\n",
                     shmClock[0], shmClock[1], deadCount);
             if(logFile) fflush(logFile);
         }
         // Select victim to terminate (process holding the most resources)
         unsigned int maxHeld = 0;
         int victimIndex = -1;
         for(int k = 0; k < deadCount; ++k) {
             int idx = deadlockedProcs[k];
             unsigned int held = 0;
             for(int r = 0; r < RES_TYPES; ++r) {
                 held += procTable[idx].alloc[r];
             }
             if(held >= maxHeld) {
                 maxHeld = held;
                 victimIndex = idx;
             }
         }
         if(victimIndex >= 0) {
             processesKilledForDeadlock++;
             if(verbose) {
                 fprintf(logFile ? logFile : stdout,
                         "OSS: Terminating process P%d (PID %d) to resolve deadlock.\n",
                         victimIndex, procTable[victimIndex].pid);
                 if(logFile) fflush(logFile);
             }
             // Send termination message to victim
             Message msg;
             msg.mtype   = procTable[victimIndex].pid;
             msg.action  = ACTION_KILL;
             msg.resource= -1;
             msg.quantity= 0;
             msg.time    = 0;
             msg.pid     = 0;
             if(msgsnd(msqid, &msg, sizeof(Message)-sizeof(long), 0) == -1) {
                 perror("oss: msgsnd ACTION_KILL");
             }
             // Free all resources held by victim (simulate immediate reclaim)
             for(int r = 0; r < RES_TYPES; ++r) {
                 if(procTable[victimIndex].alloc[r] > 0) {
                     available[r] += procTable[victimIndex].alloc[r];
                     procTable[victimIndex].alloc[r] = 0;
                 }
             }
             procTable[victimIndex].blocked = false;
             procTable[victimIndex].requestRes = -1;
             procTable[victimIndex].requestAmt = 0;
             deadlocksResolved++;
         }
     }
 }
 
 // Find an available slot in procTable for a new process
 int findFreeSlot() {
     for(int i = 0; i < MAX_PROCESSES; ++i) {
         if(!procTable[i].active) {
             return i;
         }
     }
     return -1;
 }
 
 int main(int argc, char *argv[]) {
     // Default parameters
     totalProcesses = 5;
     maxConcurrent = 2;
     quantum_ns = 100000000;  // 0.1 second default quantum
 
     // Parse command-line options
     int opt;
     while((opt = getopt(argc, argv, "n:s:b:vh")) != -1) {
         switch(opt) {
             case 'n':
                 totalProcesses = atoi(optarg);
                 break;
             case 's':
                 maxConcurrent = atoi(optarg);
                 break;
             case 'b':
                 quantum_ns = (unsigned int) atol(optarg);
                 break;
             case 'v':
                 verbose = true;
                 break;
             case 'h':
             default:
                 fprintf(stderr, "Usage: %s [-n total_procs] [-s concurrent] [-b nanosec_bound] [-v]\n", argv[0]);
                 exit(1);
         }
     }
     if(totalProcesses < 1) totalProcesses = 1;
     if(maxConcurrent < 1) maxConcurrent = 1;
     if(maxConcurrent > MAX_PROCESSES) maxConcurrent = MAX_PROCESSES;
     if(totalProcesses > MAX_PROCESSES) totalProcesses = MAX_PROCESSES;
 
     // Open log file (if verbose, log all events)
     logFile = fopen("oss.log", "w");
     if(!logFile) {
         perror("oss: fopen log");
         logFile = NULL; // fallback to stdout
     }
 
     // Set up shared memory for simulated clock
     shmid = shmget(SHMKEY, 2 * sizeof(unsigned int), IPC_CREAT | 0666);
     if(shmid == -1) {
         perror("oss: shmget");
         cleanup(0);
     }
     shmClock = (unsigned int *) shmat(shmid, NULL, 0);
     if(shmClock == (void *) -1) {
         perror("oss: shmat");
         cleanup(0);
     }
     shmClock[0] = 0;
     shmClock[1] = 0;
 
     // Set up message queue
     msqid = msgget(MSGKEY, IPC_CREAT | 0666);
     if(msqid == -1) {
         perror("oss: msgget");
         cleanup(0);
     }
 
     // Initialize resource availability and process table
     for(int r = 0; r < RES_TYPES; ++r) {
         available[r] = INSTANCES_PER_RES;
     }
     for(int i = 0; i < MAX_PROCESSES; ++i) {
         procTable[i].active = false;
         procTable[i].blocked = false;
         procTable[i].pid = 0;
         procTable[i].requestRes = -1;
         procTable[i].requestAmt = 0;
         for(int r = 0; r < RES_TYPES; ++r) {
             procTable[i].alloc[r] = 0;
         }
     }
 
     // Set up signal handler for graceful termination (Ctrl+C)
     signal(SIGINT, cleanup);
 
     // Launch initial worker processes (up to maxConcurrent or totalProcesses)
     srand(time(NULL));
     for(int count = 0; count < maxConcurrent && count < totalProcesses; ++count) {
         int slot = findFreeSlot();
         if(slot == -1) break;
         pid_t pid = fork();
         if(pid < 0) {
             perror("oss: fork");
             break;
         }
         if(pid == 0) {
             // In child process: execute worker
             char argB[32];
             sprintf(argB, "%u", quantum_ns);
             execl("./worker", "worker", argB, (char *)NULL);
             perror("oss: execl worker");
             exit(1);
         } else {
             // In parent (OSS)
             procTable[slot].active = true;
             procTable[slot].blocked = false;
             procTable[slot].pid = pid;
             procTable[slot].requestRes = -1;
             procTable[slot].requestAmt = 0;
             for(int r = 0; r < RES_TYPES; ++r) {
                 procTable[slot].alloc[r] = 0;
             }
             activeCount++;
             launchedCount++;
             if(verbose) {
                 fprintf(logFile ? logFile : stdout,
                         "OSS: Launched process P%d (PID %d)\n", slot, pid);
                 if(logFile) fflush(logFile);
             }
         }
     }
 
     unsigned int lastDeadlockCheckSec = 0;
     /* Main simulation loop */
     while(terminatedCount < totalProcesses) {
         Message msg;
         bool messageProcessed = false;
         // Process all messages from workers (non-blocking receive)
         while(msgrcv(msqid, &msg, sizeof(Message) - sizeof(long), 1, IPC_NOWAIT) != -1) {
             messageProcessed = true;
             // Identify which process sent this message (by PID field)
             pid_t wp = msg.pid;
             int procIndex = -1;
             for(int i = 0; i < MAX_PROCESSES; ++i) {
                 if(procTable[i].active && procTable[i].pid == wp) {
                     procIndex = i;
                     break;
                 }
             }
             if(procIndex == -1) {
                 continue; // unknown process (should not happen)
             }
             // Advance simulated time by the CPU time used from message
             unsigned int addTime = msg.time;
             shmClock[1] += addTime;
             if(shmClock[1] >= 1000000000) {
                 shmClock[0] += shmClock[1] / 1000000000;
                 shmClock[1] %= 1000000000;
             }
             // Handle message based on action code
             if(msg.action == ACTION_REQUEST) {
                 totalRequests++;
                 int res = msg.resource;
                 int amt = msg.quantity;
                 if(verbose) {
                     fprintf(logFile ? logFile : stdout,
                             "OSS: P%d (PID %d) requesting R%d (%d units) at time %u:%u\n",
                             procIndex, wp, res, amt, shmClock[0], shmClock[1]);
                     if(logFile) fflush(logFile);
                 }
                 if(amt <= (int)available[res]) {
                     // Resource available -> grant immediately
                     available[res] -= amt;
                     procTable[procIndex].alloc[res] += amt;
                     Message grantMsg;
                     grantMsg.mtype    = wp;
                     grantMsg.action   = ACTION_GRANT;
                     grantMsg.resource = res;
                     grantMsg.quantity = amt;
                     grantMsg.time     = 0;
                     grantMsg.pid      = 0;
                     if(msgsnd(msqid, &grantMsg, sizeof(Message) - sizeof(long), 0) == -1) {
                         perror("oss: msgsnd grant");
                     }
                     if(verbose) {
                         fprintf(logFile ? logFile : stdout,
                                 "OSS: Granted R%d (%d units) to P%d (PID %d)\n",
                                 res, amt, procIndex, wp);
                         if(logFile) fflush(logFile);
                     }
                     requestsGrantedImmediately++;
                     procTable[procIndex].blocked = false;
                     procTable[procIndex].requestRes = -1;
                     procTable[procIndex].requestAmt = 0;
                 } else {
                     // Not enough available -> block the process
                     procTable[procIndex].blocked = true;
                     procTable[procIndex].requestRes = res;
                     procTable[procIndex].requestAmt = amt;
                     requestsBlocked++;
                     if(verbose) {
                         fprintf(logFile ? logFile : stdout,
                                 "OSS: P%d (PID %d) is blocked waiting for R%d (%d units)\n",
                                 procIndex, wp, res, amt);
                         if(logFile) fflush(logFile);
                     }
                     // No response sent; worker will block waiting for grant
                 }
             } else if(msg.action == ACTION_RELEASE) {
                 totalReleases++;
                 int res = msg.resource;
                 int amt = msg.quantity;
                 // Release resources held by process
                 if(procTable[procIndex].alloc[res] < (unsigned int)amt) {
                     amt = procTable[procIndex].alloc[res];
                 }
                 procTable[procIndex].alloc[res] -= amt;
                 available[res] += amt;
                 if(verbose) {
                     fprintf(logFile ? logFile : stdout,
                             "OSS: P%d (PID %d) released R%d (%d units) at time %u:%u\n",
                             procIndex, wp, res, amt, shmClock[0], shmClock[1]);
                     if(logFile) fflush(logFile);
                 }
                 // Check if any blocked process can now be granted its request
                 for(int i = 0; i < MAX_PROCESSES; ++i) {
                     if(procTable[i].active && procTable[i].blocked) {
                         int reqRes = procTable[i].requestRes;
                         int reqAmt = procTable[i].requestAmt;
                         if(reqRes >= 0 && (unsigned int)reqAmt <= available[reqRes]) {
                             // We can satisfy this request now
                             available[reqRes] -= reqAmt;
                             procTable[i].alloc[reqRes] += reqAmt;
                             // Unblock the process
                             procTable[i].blocked = false;
                             procTable[i].requestRes = -1;
                             procTable[i].requestAmt = 0;
                             // Send grant message
                             Message grantMsg;
                             grantMsg.mtype    = procTable[i].pid;
                             grantMsg.action   = ACTION_GRANT;
                             grantMsg.resource = reqRes;
                             grantMsg.quantity = reqAmt;
                             grantMsg.time     = 0;
                             grantMsg.pid      = 0;
                             if(msgsnd(msqid, &grantMsg, sizeof(Message)-sizeof(long), 0) == -1) {
                                 perror("oss: msgsnd grant (release unblock)");
                             }
                             if(verbose) {
                                 fprintf(logFile ? logFile : stdout,
                                         "OSS: Granted R%d (%d units) to P%d (PID %d) [unblocked]\n",
                                         reqRes, reqAmt, i, procTable[i].pid);
                                 if(logFile) fflush(logFile);
                             }
                         }
                     }
                 }
             } else if(msg.action == ACTION_TERMINATE) {
                 // Worker signals it is terminating
                 if(verbose) {
                     fprintf(logFile ? logFile : stdout,
                             "OSS: P%d (PID %d) has terminated at time %u:%u\n",
                             procIndex, wp, shmClock[0], shmClock[1]);
                     if(logFile) fflush(logFile);
                 }
                 // Mark process as terminated (will be cleaned up by waitpid)
                 procTable[procIndex].active = false;
             } else if(msg.action == ACTION_NOOP) {
                 // Process used entire time slice without resource events
                 if(verbose) {
                     fprintf(logFile ? logFile : stdout,
                             "OSS: P%d (PID %d) used full time slice at time %u:%u\n",
                             procIndex, wp, shmClock[0], shmClock[1]);
                     if(logFile) fflush(logFile);
                 }
             }
         }
         // Deadlock detection check every simulated second
         if(shmClock[0] > lastDeadlockCheckSec) {
             lastDeadlockCheckSec = shmClock[0];
             detectDeadlock();
         }
         // If no message processed and processes are idle/blocked, advance time to trigger deadlock check
         if(!messageProcessed && activeCount > 0) {
             unsigned int inc = 100000000; // 100ms
             if(shmClock[0] == lastDeadlockCheckSec) {
                 if(shmClock[1] + inc >= 1000000000) {
                     shmClock[0] += 1;
                     shmClock[1] = 0;
                 } else {
                     shmClock[1] += inc;
                 }
             }
         }
         // Check for any finished child processes (non-blocking wait)
         int status = 0;
         pid_t pid;
         while((pid = waitpid(-1, &status, WNOHANG)) > 0) {
             // Find which process ended
             int idx = -1;
             for(int i = 0; i < MAX_PROCESSES; ++i) {
                 if(procTable[i].active && procTable[i].pid == pid) {
                     idx = i;
                     break;
                 }
             }
             if(idx == -1) {
                 // If not marked active (perhaps already set inactive on terminate message)
                 for(int i = 0; i < MAX_PROCESSES; ++i) {
                     if(procTable[i].pid == pid) {
                         idx = i;
                         break;
                     }
                 }
             }
             if(idx != -1) {
                 // Free any resources still allocated to this process
                 for(int r = 0; r < RES_TYPES; ++r) {
                     if(procTable[idx].alloc[r] > 0) {
                         available[r] += procTable[idx].alloc[r];
                         procTable[idx].alloc[r] = 0;
                     }
                 }
                 // Clear blocked status and pending request if any
                 procTable[idx].blocked = false;
                 procTable[idx].requestRes = -1;
                 procTable[idx].requestAmt = 0;
                 procTable[idx].active = false;
                 activeCount--;
                 terminatedCount++;
                 if(verbose) {
                     fprintf(logFile ? logFile : stdout,
                             "OSS: Cleaned up P%d (PID %d). Total terminated: %d\n",
                             idx, pid, terminatedCount);
                     if(logFile) fflush(logFile);
                 }
             }
             // Launch a new process if we still have more to start
             if(launchedCount < totalProcesses && activeCount < maxConcurrent) {
                 int slot = findFreeSlot();
                 if(slot != -1) {
                     pid_t npid = fork();
                     if(npid < 0) {
                         perror("oss: fork");
                     } else if(npid == 0) {
                         char argB[32];
                         sprintf(argB, "%u", quantum_ns);
                         execl("./worker", "worker", argB, (char*)NULL);
                         perror("oss: execl new worker");
                         exit(1);
                     } else {
                         procTable[slot].active = true;
                         procTable[slot].blocked = false;
                         procTable[slot].pid = npid;
                         procTable[slot].requestRes = -1;
                         procTable[slot].requestAmt = 0;
                         for(int r = 0; r < RES_TYPES; ++r) {
                             procTable[slot].alloc[r] = 0;
                         }
                         activeCount++;
                         launchedCount++;
                         if(verbose) {
                             fprintf(logFile ? logFile : stdout,
                                     "OSS: Launched new process P%d (PID %d)\n",
                                     slot, npid);
                             if(logFile) fflush(logFile);
                         }
                     }
                 }
             }
         }
         // Dispatch all active and ready (not blocked) processes for the next time slice
         for(int i = 0; i < MAX_PROCESSES; ++i) {
             if(procTable[i].active && !procTable[i].blocked) {
                 Message dispatchMsg;
                 dispatchMsg.mtype    = procTable[i].pid;
                 dispatchMsg.action   = ACTION_DISPATCH;
                 dispatchMsg.resource = -1;
                 dispatchMsg.quantity = 0;
                 dispatchMsg.time     = quantum_ns;
                 dispatchMsg.pid      = 0;
                 if(msgsnd(msqid, &dispatchMsg, sizeof(Message) - sizeof(long), 0) == -1) {
                     perror("oss: msgsnd dispatch");
                 }
             }
         }
     }
 
     // Simulation done - output summary statistics
     fprintf(logFile ? logFile : stdout, "\nOSS: Simulation complete. Statistics:\n");
     fprintf(logFile ? logFile : stdout, "Total processes: %d (max concurrent %d)\n", totalProcesses, maxConcurrent);
     fprintf(logFile ? logFile : stdout, "Total resource requests: %lu (granted immediately: %lu, blocked: %lu)\n",
             totalRequests, requestsGrantedImmediately, requestsBlocked);
     fprintf(logFile ? logFile : stdout, "Total resource releases: %lu\n", totalReleases);
     fprintf(logFile ? logFile : stdout, "Deadlock occurrences detected: %lu\n", deadlockDetections);
     fprintf(logFile ? logFile : stdout, "Processes terminated to resolve deadlocks: %lu\n", processesKilledForDeadlock);
 
     if(logFile) fclose(logFile);
     // Cleanup IPC resources
     msgctl(msqid, IPC_RMID, NULL);
     shmdt(shmClock);
     shmctl(shmid, IPC_RMID, NULL);
     return 0;
 }
 