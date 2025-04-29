/*
 * worker.c - Worker process for OS Simulator
 * Simulates a user process that requests/releases resources and can terminate.
 */

 #include <stdio.h>
 #include <stdlib.h>
 #include <unistd.h>
 #include <sys/types.h>
 #include <sys/ipc.h>
 #include <sys/shm.h>
 #include <sys/msg.h>
 #include <signal.h>
 #include <string.h>
 #include <errno.h>
 #include <time.h>
 #include <stdbool.h>
 
 #define SHMKEY 0x98765  // Must match OSS
 #define MSGKEY 0x12345  // Must match OSS
 
 #define RES_TYPES 5
 
 // Message structure (must match definition in oss.c)
 typedef struct {
     long mtype;
     int action;
     int resource;
     int quantity;
     unsigned int time;
     pid_t pid;
 } Message;
 
 // Action codes (same values as in OSS)
 #define ACTION_REQUEST   1
 #define ACTION_RELEASE   2
 #define ACTION_TERMINATE 3
 #define ACTION_NOOP      4
 #define ACTION_DISPATCH  1
 #define ACTION_GRANT     2
 #define ACTION_KILL      3
 
 int shmid = -1;
 unsigned int *shmClock = NULL;
 int msqid = -1;
 
 // Cleanup shared memory attachment
 void cleanup() {
     if(shmClock != (void*)-1 && shmClock != NULL) {
         shmdt(shmClock);
     }
 }
 
 // Signal handler for graceful exit
 void handleSig(int sig) {
     (void)sig;
     cleanup();
     exit(0);
 }
 
 int main(int argc, char *argv[]) {
     (void)argv;
     // The bound B (quantum in ns) can be passed in argv[1], but we use dispatch message from OSS for timing.
     srand(time(NULL) ^ (getpid() << 16));
 
     // Attach to existing shared memory clock
     shmid = shmget(SHMKEY, 2 * sizeof(unsigned int), 0666);
     if(shmid == -1) {
         perror("worker: shmget");
         return 1;
     }
     shmClock = (unsigned int*) shmat(shmid, NULL, 0);
     if(shmClock == (unsigned int*) -1) {
         perror("worker: shmat");
         return 1;
     }
     // Connect to the message queue
     msqid = msgget(MSGKEY, 0666);
     if(msqid == -1) {
         perror("worker: msgget");
         cleanup();
         return 1;
     }
     // Handle termination signals
     signal(SIGINT, handleSig);
     signal(SIGTERM, handleSig);
 
     // Resource instances held by this process
     unsigned int held[RES_TYPES] = {0};
     Message msg;
     /* Wait for messages from OSS (dispatch or resource grant/termination) */
     while(true) {
         // Wait for a message directed to this PID
         if(msgrcv(msqid, &msg, sizeof(Message) - sizeof(long), getpid(), 0) == -1) {
             if(errno == EINTR) continue;
             perror("worker: msgrcv");
             cleanup();
             return 1;
         }
         if(msg.action == ACTION_DISPATCH) {
             // Received a dispatch (time slice) from OSS
             unsigned int timeSlice = msg.time;
             // Determine action for this time slice
             double p = (double) rand() / RAND_MAX;
             double probTerminate = 0.1;
             double probRequest   = 0.5;
             double probRelease   = 0.3;
             // Adjust probabilities if holding no resources (can't release)
             if((held[0] + held[1] + held[2] + held[3] + held[4]) == 0) {
                 probRequest = 0.6;
                 probRelease = 0.0;
             }
             unsigned int usedTime = 0;
             if(p < probTerminate) {
                 // Decide to terminate in this time slice
                 // Release all held resources before terminating
                 for(int r = 0; r < RES_TYPES; ++r) {
                     if(held[r] > 0) {
                         Message relMsg;
                         relMsg.mtype    = 1;
                         relMsg.action   = ACTION_RELEASE;
                         relMsg.resource = r;
                         relMsg.quantity = held[r];
                         relMsg.time     = 0;
                         relMsg.pid      = getpid();
                         msgsnd(msqid, &relMsg, sizeof(Message) - sizeof(long), 0);
                         held[r] = 0;
                     }
                 }
                 // Simulate using some CPU time before termination
                 usedTime = (timeSlice > 0) ? (rand() % (int)timeSlice) : 0;
                 // Notify OSS that this process is terminating
                 Message termMsg;
                 termMsg.mtype    = 1;
                 termMsg.action   = ACTION_TERMINATE;
                 termMsg.resource = -1;
                 termMsg.quantity = 0;
                 termMsg.time     = usedTime;
                 termMsg.pid      = getpid();
                 msgsnd(msqid, &termMsg, sizeof(Message) - sizeof(long), 0);
                 break; // exit loop to terminate
             } else if(p < probTerminate + probRequest) {
                 // Decide to make a resource request
                 int res = rand() % RES_TYPES;
                 int amt = (rand() % 3) + 1;
                 // Ensure request does not exceed system limit (10) minus what we already hold
                 if(amt + held[res] > 10) {
                     amt = 10 - held[res];
                     if(amt < 1) amt = 1;
                 }
                 // Simulate some CPU time before making the request
                 usedTime = (timeSlice > 0) ? (rand() % (int)timeSlice) : 0;
                 // Send resource request message to OSS
                 Message reqMsg;
                 reqMsg.mtype    = 1;
                 reqMsg.action   = ACTION_REQUEST;
                 reqMsg.resource = res;
                 reqMsg.quantity = amt;
                 reqMsg.time     = usedTime;
                 reqMsg.pid      = getpid();
                 if(msgsnd(msqid, &reqMsg, sizeof(Message) - sizeof(long), 0) == -1) {
                     perror("worker: msgsnd request");
                 }
                 // Wait for OSS to grant resource or terminate this process (if deadlock recovery)
                 if(msgrcv(msqid, &msg, sizeof(Message) - sizeof(long), getpid(), 0) == -1) {
                     if(errno == EINTR) continue;
                     perror("worker: msgrcv waiting for grant");
                     break;
                 }
                 if(msg.action == ACTION_GRANT) {
                     // Resource granted by OSS
                     held[res] += amt;
                     // End of time slice (we blocked waiting, so no further CPU this slice)
                 } else if(msg.action == ACTION_KILL) {
                     // OSS killed this process (deadlock resolution)
                     break;
                 }
             } else if(p < probTerminate + probRequest + probRelease) {
                 // Decide to release a resource during this slice
                 if(held[0] + held[1] + held[2] + held[3] + held[4] > 0) {
                     // Choose a resource that we currently hold
                     int resList[RES_TYPES];
                     int resCount = 0;
                     for(int r = 0; r < RES_TYPES; ++r) {
                         if(held[r] > 0) {
                             resList[resCount++] = r;
                         }
                     }
                     int res = resList[rand() % resCount];
                     // Release a random amount (at least 1, at most all held of that type)
                     int amt = (rand() % held[res]) + 1;
                     // Simulate some CPU time before releasing
                     usedTime = (timeSlice > 0) ? (unsigned int)(rand() % (int)timeSlice) : 0;
                     // Send release message to OSS
                     Message relMsg;
                     relMsg.mtype    = 1;
                     relMsg.action   = ACTION_RELEASE;
                     relMsg.resource = res;
                     relMsg.quantity = amt;
                     relMsg.time     = usedTime;
                     relMsg.pid      = getpid();
                     msgsnd(msqid, &relMsg, sizeof(Message) - sizeof(long), 0);
                     // Update local held count
                     if(amt > (int)held[res]) amt = held[res];
                     held[res] -= amt;
                     // Yield the CPU after releasing (end of time slice)
                 } else {
                     // If we have nothing to release, just consume the whole time slice
                     usedTime = timeSlice;
                     Message noopMsg;
                     noopMsg.mtype    = 1;
                     noopMsg.action   = ACTION_NOOP;
                     noopMsg.resource = -1;
                     noopMsg.quantity = 0;
                     noopMsg.time     = usedTime;
                     noopMsg.pid      = getpid();
                     msgsnd(msqid, &noopMsg, sizeof(Message) - sizeof(long), 0);
                 }
             } else {
                 // No special action: use entire quantum for computation (no request or release)
                 usedTime = timeSlice;
                 Message noopMsg;
                 noopMsg.mtype    = 1;
                 noopMsg.action   = ACTION_NOOP;
                 noopMsg.resource = -1;
                 noopMsg.quantity = 0;
                 noopMsg.time     = usedTime;
                 noopMsg.pid      = getpid();
                 msgsnd(msqid, &noopMsg, sizeof(Message) - sizeof(long), 0);
             }
             // Loop back to wait for next dispatch or termination
         } else if(msg.action == ACTION_GRANT) {
             // Received an unexpected grant (should only happen when waiting in the block above)
             int res = msg.resource;
             int amt = msg.quantity;
             held[res] += amt;
             // Continue waiting for dispatch
         } else if(msg.action == ACTION_KILL) {
             // OSS requests this process to terminate (deadlock victim)
             break;
         }
     }
     // Cleanup and exit
     cleanup();
     return 0;
 }