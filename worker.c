/*
 * worker.c
 * This program simulates a user process that waits for a scheduling message
 * from OSS and then “runs” for a specified time quantum.
 * Using randomness, the worker decides one of three outcomes:
 *   - Terminates early (sending a negative response with the time used)
 *   - Uses only part of its quantum (simulating an I/O block)
 *   - Uses its full allocated quantum
 *
 * Communication with OSS is done via a System V message queue.
 *
  * Author: [Jad Aqrabawi]
 * Date: [14/04/2025]
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
 #include <stdbool.h>
 #include <time.h>
 
 #define SHMKEY 9876
 #define MSGKEY 1234
 
 // Outcome probabilities
 #define P_TERMINATE 0.1  // 10% chance to terminate
 #define P_BLOCK 0.3      // 30% chance to block on I/O
 
 typedef struct {
     long mtype;  // message type
     int mtext;   // message content (quantum or response)
 } Message;
 
 int shmid;
 int *shmClock;
 int msqid;
 
 void cleanup() {
     if (shmClock != (void *) -1) {
         shmdt(shmClock);
     }
 }
 
 int main() {
     srand(time(NULL) ^ (getpid()<<16));  // Seed uniquely for each process
 
     // Attach to the shared memory clock.
     shmid = shmget(SHMKEY, 2 * sizeof(int), 0666);
     if (shmid == -1) {
         perror("worker: shmget");
         exit(1);
     }
     shmClock = (int *) shmat(shmid, NULL, 0);
     if (shmClock == (int *) -1) {
         perror("worker: shmat");
         exit(1);
     }
     
     // Access the message queue.
     msqid = msgget(MSGKEY, 0666);
     if (msqid == -1) {
         perror("worker: msgget");
         cleanup();
         exit(1);
     }
     
     while (true) {
         Message msg;
         // Wait for a message addressed specifically to this worker (using its PID).
         if (msgrcv(msqid, &msg, sizeof(msg.mtext), getpid(), 0) == -1) {
             perror("worker: msgrcv");
             cleanup();
             exit(1);
         }
         
         int timeQuantum = msg.mtext;
         int usedTime = 0;
         double p = (double) rand() / RAND_MAX;
         
         if (p < P_TERMINATE) {
             // Terminate after using part of the quantum.
             usedTime = (timeQuantum > 1) ? 1 + rand() % (timeQuantum - 1) : timeQuantum;
             msg.mtext = -usedTime;  // Negative indicates termination.
             printf("WORKER PID:%d: Terminating after using %d ns (quantum %d ns).\n",
                    getpid(), usedTime, timeQuantum);
             msg.mtype = getppid();
             if (msgsnd(msqid, &msg, sizeof(msg.mtext), 0) == -1) {
                 perror("worker: msgsnd");
                 cleanup();
                 exit(1);
             }
             break;
         } else if (p < P_TERMINATE + P_BLOCK) {
             // Simulate an I/O block by using part of the quantum.
             usedTime = (timeQuantum > 1) ? 1 + rand() % (timeQuantum - 1) : timeQuantum;
             msg.mtext = usedTime;
             printf("WORKER PID:%d: Blocking on I/O after using %d ns (quantum %d ns).\n",
                    getpid(), usedTime, timeQuantum);
         } else {
             // Use the full quantum.
             usedTime = timeQuantum;
             msg.mtext = timeQuantum;
             printf("WORKER PID:%d: Using full quantum of %d ns.\n", getpid(), timeQuantum);
         }
         
         msg.mtype = getppid();
         if (msgsnd(msqid, &msg, sizeof(msg.mtext), 0) == -1) {
             perror("worker: msgsnd");
             cleanup();
             exit(1);
         }
     }
     
     cleanup();
     return 0;
 }
 