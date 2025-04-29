/*
 * oss.c
 * OS Scheduler Simulator using Multi-Level Feedback Queue (MLFQ) and message queues.
 * This program simulates process scheduling in an operating system.
 * It launches child processes at random simulated intervals, dispatches them using an MLFQ
 * (with 3 levels), and logs scheduling events along with detailed process table and queue states.
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
 #include <sys/wait.h>
 #include <signal.h>
 #include <time.h>
 #include <string.h>
 #include <errno.h>
 #include <stdbool.h>
 #include <getopt.h>
 
 #define SHMKEY 9876
 #define MSGKEY 1234
 
 #define MAX_CHILDREN 20        // Process table size
 #define ONE_BILLION 1000000000ULL
 
 // Default simulation parameters
 #define DEFAULT_TOTAL_PROCS 100
 #define DEFAULT_SIMUL_LIMIT 18
 #define DEFAULT_LAUNCH_INTERVAL_MS 100  // Fallback interval if needed
 #define BASE_QUANTUM 10000000  // Base quantum: 10ms in nanoseconds
 
 // For random process launching interval: up to 1 second (0-1 sec, in ns)
 #define MAX_LAUNCH_SEC 1
 #define MAX_LAUNCH_NS 0
 
 // Process Control Block (PCB)
 typedef struct {
     int occupied;              // 1 if used, 0 if free
     pid_t pid;                 // Process id
     int startSeconds;          // Simulated start seconds
     int startNano;             // Simulated start nanoseconds
     int serviceTimeSeconds;    // Total seconds scheduled (not computed in detail)
     int serviceTimeNano;       // Total nanoseconds scheduled (not computed in detail)
     int eventWaitSec;          // When the event for a blocked process happens (seconds)
     int eventWaitNano;         // When the event for a blocked process happens (nanoseconds)
     int blocked;               // 1 if blocked, 0 if ready
     int messagesSent;          // Number of messages sent to this process
     int currentQueueLevel;     // Current MLFQ level (0 = highest, 1 = medium, 2 = lowest)
     int totalCpuTime;          // Total CPU time used by the process (ns)
     unsigned long long readyTime; // Simulated time (in ns) when process became ready
 } PCB;
 
 PCB processTable[MAX_CHILDREN];
 
 int shmid;
 int *shmClock;
 int msqid;
 
 int totalProcs = DEFAULT_TOTAL_PROCS;
 int simulLimit = DEFAULT_SIMUL_LIMIT;
 int launchIntervalMs = DEFAULT_LAUNCH_INTERVAL_MS;
 char logFileName[256] = "oss.log";
 
 // Statistics global variables:
 unsigned long long totalWaitTime = 0;
 int scheduleCount = 0;
 unsigned long long totalCpuUsage = 0;
 unsigned long long totalIdleTime = 0;
 unsigned long long totalBlockedWaitTime = 0;
 int blockedEventCount = 0;
 int queueCount[3] = {0, 0, 0};
 
 int logLineCount = 0;  // Count of log lines (limit 10000)
 
 volatile sig_atomic_t terminateFlag = 0;
 
 // Cleanup routine: detach and remove IPC resources, then kill processes.
 void cleanup(int signum) {
     if (shmClock != (void *) -1) {
         shmdt(shmClock);
     }
     shmctl(shmid, IPC_RMID, NULL);
     msgctl(msqid, IPC_RMID, NULL);
     kill(0, SIGTERM);
     exit(1);
 }
 
 // SIGALRM handler to enforce real-time termination (3 seconds).
 void alarmHandler(int signum) {
     printf("Real time limit reached. Terminating oss and all children.\n");
     cleanup(signum);
 }
 
 // Increment simulated clock by specified nanoseconds.
 void incrementClockByNS(unsigned int nsIncrement) {
     shmClock[1] += nsIncrement;
     if (shmClock[1] >= ONE_BILLION) {
         shmClock[0] += shmClock[1] / ONE_BILLION;
         shmClock[1] %= ONE_BILLION;
     }
 }
 
 // Simulate dispatch overhead by generating a random delay (100 to 10,000 ns),
 // increment the clock by this overhead, and return the overhead value.
 int dispatchOverhead() {
     int overhead = (rand() % (10000 - 100 + 1)) + 100;
     incrementClockByNS(overhead);
     return overhead;
 }
 
 // Compare simulated time values.
 bool timeGreaterOrEqual(int sec1, int nano1, int sec2, int nano2) {
     if (sec1 > sec2) return true;
     if (sec1 == sec2 && nano1 >= nano2) return true;
     return false;
 }
 
 // Output the current simulated time, process table, and an overview of the queues to the log.
 void displayTimeAndQueues(FILE *logFilePtr) {
     fprintf(logFilePtr, "OSS PID: %d | SysClock: %d s, %d ns\n", getpid(), shmClock[0], shmClock[1]);
     logLineCount++;
     fprintf(logFilePtr, "Process Table:\n");
     fprintf(logFilePtr, "Idx Occupied PID     StartSec StartNano MsgSent QLevel CPUTime(ns) Blocked ReadyTime(ns)\n");
     logLineCount++;
     for (int i = 0; i < MAX_CHILDREN; i++) {
         fprintf(logFilePtr, "%-4d %-8d %-7d %-8d %-9d %-7d %-6d %-12d %-7d %-14llu\n",
                 i, processTable[i].occupied, processTable[i].pid,
                 processTable[i].startSeconds, processTable[i].startNano,
                 processTable[i].messagesSent, processTable[i].currentQueueLevel,
                 processTable[i].totalCpuTime, processTable[i].blocked,
                 processTable[i].readyTime);
         logLineCount++;
     }
     // Output the contents of each queue.
     fprintf(logFilePtr, "OSS: Outputting queues:\n");
     logLineCount++;
     fprintf(logFilePtr, "q0: ");
     logLineCount++;
     for (int i = 0; i < MAX_CHILDREN; i++) {
         if (processTable[i].occupied && !processTable[i].blocked && processTable[i].currentQueueLevel == 0)
             fprintf(logFilePtr, "P%d ", processTable[i].pid);
     }
     fprintf(logFilePtr, "\nq1: ");
     logLineCount++;
     for (int i = 0; i < MAX_CHILDREN; i++) {
         if (processTable[i].occupied && !processTable[i].blocked && processTable[i].currentQueueLevel == 1)
             fprintf(logFilePtr, "P%d ", processTable[i].pid);
     }
     fprintf(logFilePtr, "\nq2: ");
     logLineCount++;
     for (int i = 0; i < MAX_CHILDREN; i++) {
         if (processTable[i].occupied && !processTable[i].blocked && processTable[i].currentQueueLevel == 2)
             fprintf(logFilePtr, "P%d ", processTable[i].pid);
     }
     fprintf(logFilePtr, "\nblocked: ");
     logLineCount++;
     for (int i = 0; i < MAX_CHILDREN; i++) {
         if (processTable[i].occupied && processTable[i].blocked)
             fprintf(logFilePtr, "P%d ", processTable[i].pid);
     }
     fprintf(logFilePtr, "\n\n");
     logLineCount++;
     fflush(logFilePtr);
 }
 
 int main(int argc, char *argv[]) {
     int opt;
     while ((opt = getopt(argc, argv, "hn:s:i:f:")) != -1) {
         switch (opt) {
             case 'h':
                 printf("Usage: %s [-n totalProcs] [-s simulLimit] [-i launchIntervalMs] [-f logfile]\n", argv[0]);
                 exit(0);
             case 'n':
                 totalProcs = atoi(optarg);
                 break;
             case 's':
                 simulLimit = atoi(optarg);
                 break;
             case 'i':
                 launchIntervalMs = atoi(optarg);
                 break;
             case 'f':
                 strncpy(logFileName, optarg, sizeof(logFileName) - 1);
                 break;
             default:
                 fprintf(stderr, "Unknown option: %c\n", opt);
                 exit(1);
         }
     }
     
     signal(SIGINT, cleanup);
     signal(SIGALRM, alarmHandler);
     alarm(3); // Terminate after 3 real-life seconds
     
     // Set up shared memory for the simulated clock.
     shmid = shmget(SHMKEY, 2 * sizeof(int), IPC_CREAT | 0666);
     if (shmid == -1) { perror("oss: shmget"); exit(1); }
     shmClock = (int *) shmat(shmid, NULL, 0);
     if (shmClock == (int *) -1) { perror("oss: shmat"); exit(1); }
     shmClock[0] = 0;
     shmClock[1] = 0;
     
     // Initialize the process table.
     for (int i = 0; i < MAX_CHILDREN; i++) {
         processTable[i].occupied = 0;
     }
     
     // Create a message queue.
     msqid = msgget(MSGKEY, IPC_CREAT | 0666);
     if (msqid == -1) { perror("oss: msgget"); cleanup(0); }
     
     FILE *logFilePtr = fopen(logFileName, "w");
     if (!logFilePtr) { perror("oss: fopen"); cleanup(0); }
     
     int launchedCount = 0;
     int runningCount = 0;
     unsigned long long lastLaunchTime = 0;
     unsigned long long lastTableDisplayTime = 0;
     
     srand(time(NULL));
     
     // Main simulation loop.
     while ((launchedCount < totalProcs || runningCount > 0) && logLineCount < 10000) {
         // Increment the simulated clock (using a small value per iteration).
         incrementClockByNS(runningCount == 0 ? 1 : (250000000 / runningCount));
         unsigned long long currentSimTime = ((unsigned long long) shmClock[0]) * ONE_BILLION + shmClock[1];
         
         // Every 0.5 simulated seconds, output the process table and queue status.
         if (currentSimTime - lastTableDisplayTime >= 500000000ULL) {
             displayTimeAndQueues(logFilePtr);
             lastTableDisplayTime = currentSimTime;
         }
         
         // Check for terminated child processes (non-blocking).
         int status;
         pid_t pidTerm = waitpid(-1, &status, WNOHANG);
         if (pidTerm > 0) {
             for (int i = 0; i < MAX_CHILDREN; i++) {
                 if (processTable[i].occupied && processTable[i].pid == pidTerm) {
                     processTable[i].occupied = 0;
                     runningCount--;
                     fprintf(logFilePtr, "OSS: Child PID %d terminated at time %d:%d.\n", pidTerm, shmClock[0], shmClock[1]);
                     logLineCount++;
                     fflush(logFilePtr);
                     break;
                 }
             }
         }
         
         // Launch a new process if allowed.
         // Generate a random interval (0 to 1 second) in nanoseconds.
         unsigned long long randomInterval = rand() % (1 * ONE_BILLION + 1);
         if (launchedCount < totalProcs && runningCount < simulLimit &&
             (currentSimTime - lastLaunchTime) >= randomInterval) {
             int slot = -1;
             for (int i = 0; i < MAX_CHILDREN; i++) {
                 if (!processTable[i].occupied) { slot = i; break; }
             }
             if (slot != -1) {
                 pid_t pid = fork();
                 if (pid < 0) { perror("oss: fork"); cleanup(0); }
                 else if (pid == 0) {
                     execl("./worker", "worker", (char *) NULL);
                     perror("oss: execl");
                     exit(1);
                 } else {
                     processTable[slot].occupied = 1;
                     processTable[slot].pid = pid;
                     processTable[slot].startSeconds = shmClock[0];
                     processTable[slot].startNano = shmClock[1];
                     processTable[slot].messagesSent = 0;
                     processTable[slot].currentQueueLevel = 0;
                     processTable[slot].totalCpuTime = 0;
                     processTable[slot].blocked = 0;
                     processTable[slot].eventWaitSec = 0;
                     processTable[slot].eventWaitNano = 0;
                     processTable[slot].readyTime = currentSimTime;
                     launchedCount++;
                     runningCount++;
                     lastLaunchTime = currentSimTime;
                     fprintf(logFilePtr, "OSS: Generating process with PID %d and putting it in queue 0 at time %d:%d.\n",
                             pid, shmClock[0], shmClock[1]);
                     logLineCount++;
                     displayTimeAndQueues(logFilePtr);
                 }
             }
         }
         
         // Check blocked processes; if their event wait time has passed, wake them up.
         for (int i = 0; i < MAX_CHILDREN; i++) {
             if (processTable[i].occupied && processTable[i].blocked) {
                 unsigned long long wakeTime = ((unsigned long long) processTable[i].eventWaitSec) * ONE_BILLION + processTable[i].eventWaitNano;
                 if (currentSimTime >= wakeTime) {
                     processTable[i].blocked = 0;
                     processTable[i].currentQueueLevel = 0; // Woken processes return to highest priority.
                     processTable[i].readyTime = wakeTime;
                     fprintf(logFilePtr, "OSS: Waking up worker PID %d from blocked queue at time %d:%d.\n",
                             processTable[i].pid, shmClock[0], shmClock[1]);
                     logLineCount++;
                 }
             }
         }
         
         // Select the ready process (not blocked) from the highest priority queue.
         int selectedIndex = -1;
         int selectedLevel = 3;
         for (int i = 0; i < MAX_CHILDREN; i++) {
             if (processTable[i].occupied && !processTable[i].blocked) {
                 if (processTable[i].currentQueueLevel < selectedLevel) {
                     selectedLevel = processTable[i].currentQueueLevel;
                     selectedIndex = i;
                 }
             }
         }
         
         if (selectedIndex == -1) {
             // No ready processes: increment clock by 100ms and add to idle time.
             incrementClockByNS(100000000);
             totalIdleTime += 100000000;
             continue;
         }
         
         // Dispatch the selected process.
         unsigned long long waitTime = currentSimTime - processTable[selectedIndex].readyTime;
         totalWaitTime += waitTime;
         scheduleCount++;
         
         fprintf(logFilePtr, "OSS: Dispatching process with PID %d from queue %d at time %d:%d.\n",
                 processTable[selectedIndex].pid, processTable[selectedIndex].currentQueueLevel, shmClock[0], shmClock[1]);
         logLineCount++;
         
         int overhead = dispatchOverhead();
         fprintf(logFilePtr, "OSS: Total time this dispatch overhead was %d nanoseconds.\n", overhead);
         logLineCount++;
         
         int quantum;
         if (processTable[selectedIndex].currentQueueLevel == 0)
             quantum = BASE_QUANTUM;
         else if (processTable[selectedIndex].currentQueueLevel == 1)
             quantum = 2 * BASE_QUANTUM;
         else
             quantum = 4 * BASE_QUANTUM;
         
         // Send the quantum to the worker.
         typedef struct {
             long mtype;
             int mtext;
         } Message;
         Message msg;
         msg.mtype = processTable[selectedIndex].pid;
         msg.mtext = quantum;
         if (msgsnd(msqid, &msg, sizeof(msg.mtext), 0) == -1) {
             perror("oss: msgsnd");
             cleanup(0);
         }
         processTable[selectedIndex].messagesSent++;
         
         // Wait for the worker's response.
         if (msgrcv(msqid, &msg, sizeof(msg.mtext), getpid(), 0) == -1) {
             perror("oss: msgrcv");
             cleanup(0);
         }
         int response = msg.mtext;
         processTable[selectedIndex].totalCpuTime += (response < 0 ? -response : response);
         
         if (response < 0) {
             // Process terminated.
             fprintf(logFilePtr, "OSS: Receiving that process with PID %d ran for %d nanoseconds (termination).\n",
                     processTable[selectedIndex].pid, -response);
             logLineCount++;
             fprintf(logFilePtr, "OSS: Removing process with PID %d from system.\n",
                     processTable[selectedIndex].pid);
             logLineCount++;
             waitpid(processTable[selectedIndex].pid, NULL, 0);
             processTable[selectedIndex].occupied = 0;
             runningCount--;
         } else if (response == quantum) {
             // Full quantum used: process is demoted if not already at lowest level.
             fprintf(logFilePtr, "OSS: Receiving that process with PID %d ran for %d nanoseconds (full quantum).\n",
                     processTable[selectedIndex].pid, quantum);
             logLineCount++;
             if (processTable[selectedIndex].currentQueueLevel < 2) {
                 processTable[selectedIndex].currentQueueLevel++;
                 fprintf(logFilePtr, "OSS: Demoting process with PID %d to queue level %d.\n",
                         processTable[selectedIndex].pid, processTable[selectedIndex].currentQueueLevel);
                 logLineCount++;
             } else {
                 fprintf(logFilePtr, "OSS: Process with PID %d remains in lowest queue level.\n",
                         processTable[selectedIndex].pid);
                 logLineCount++;
             }
             queueCount[processTable[selectedIndex].currentQueueLevel]++;
             // Process remains ready: update ready time.
             processTable[selectedIndex].readyTime = ((unsigned long long) shmClock[0]) * ONE_BILLION + shmClock[1];
         } else if (response > 0 && response < quantum) {
             // Process used part of its quantum and is blocked.
             fprintf(logFilePtr, "OSS: Receiving that process with PID %d ran for %d nanoseconds (partial quantum, blocking).\n",
                     processTable[selectedIndex].pid, response);
             logLineCount++;
             blockedEventCount++;
             // Calculate random block wait time: seconds from [0,5] and nanoseconds from [0,1000].
             int waitSec = rand() % 6;
             int waitNano = rand() % 1001;
             unsigned long long current = ((unsigned long long) shmClock[0]) * ONE_BILLION + shmClock[1];
             unsigned long long wakeTime = current + ((unsigned long long)waitSec) * ONE_BILLION + waitNano;
             processTable[selectedIndex].blocked = 1;
             processTable[selectedIndex].eventWaitSec = wakeTime / ONE_BILLION;
             processTable[selectedIndex].eventWaitNano = wakeTime % ONE_BILLION;
             totalBlockedWaitTime += (wakeTime - current);
             fprintf(logFilePtr, "OSS: Putting process with PID %d into blocked queue; will wake at %d:%d.\n",
                     processTable[selectedIndex].pid, processTable[selectedIndex].eventWaitSec, processTable[selectedIndex].eventWaitNano);
             logLineCount++;
         }
     }
     
     // Simulation end: output final statistics.
     unsigned long long finalSimTime = ((unsigned long long) shmClock[0]) * ONE_BILLION + shmClock[1];
     double avgWaitTime = (scheduleCount > 0) ? ((double) totalWaitTime / scheduleCount) : 0;
     double cpuUtilization = (finalSimTime > 0) ? (((double) totalCpuUsage / finalSimTime) * 100) : 0;
     double avgBlockedWait = (blockedEventCount > 0) ? ((double) totalBlockedWaitTime / blockedEventCount) : 0;
     
     fprintf(logFilePtr, "\nOSS: Simulation Summary:\n");
     fprintf(logFilePtr, "Total processes launched: %d\n", launchedCount);
     fprintf(logFilePtr, "Average wait time per dispatch: %.2f ns\n", avgWaitTime);
     fprintf(logFilePtr, "CPU Utilization: %.2f%%\n", cpuUtilization);
     fprintf(logFilePtr, "Average blocked wait time: %.2f ns\n", avgBlockedWait);
     fprintf(logFilePtr, "Total CPU idle time: %llu ns\n", totalIdleTime);
     fprintf(logFilePtr, "Queue scheduling counts: Level0: %d, Level1: %d, Level2: %d, Blocked events: %d\n",
             queueCount[0], queueCount[1], queueCount[2], blockedEventCount);
     fclose(logFilePtr);
     
     shmdt(shmClock);
     shmctl(shmid, IPC_RMID, NULL);
     msgctl(msqid, IPC_RMID, NULL);
     
     return 0;
 }
 