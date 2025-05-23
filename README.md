
# OS Resource Manager Simulator

## Project Overview

This C project simulates an operating system’s resource manager with deadlock detection and recovery. The main **`oss`** (Operating System Simulator) process spawns multiple worker (user) processes. Each worker randomly requests and releases system resources while the **`oss`** process tracks resource allocation and enforces mutual exclusion. The **`oss`** process periodically runs a deadlock detection algorithm to find cycles of processes waiting on each other. If a deadlock is detected, **`oss`** resolves it by terminating one or more involved processes, which frees up resources for others. This mimics real operating systems where deadlocks (situations in which processes are each waiting for resources held by others) are resolved by recovery techniques such as process termination. In summary, this simulation ensures that limited resources (CPU, memory, I/O devices) are allocated to processes efficiently while preventing deadlock conditions 

## System Requirements

- **GCC** compiler (version 4.5 or later recommended) that supports C99 or a newer standard. Modern GCC fully supports the C99 standard when invoked with `-std=c99` 
- **UNIX/Linux** operating system (POSIX-compliant).  POSIX (Portable Operating System Interface) specifies a standard environment for UNIX-like systems, ensuring portability of applications across compliant OSS
- **make** utility (GNU Make or compatible). GNU Make is a standard build tool on Unix-like systems; it automatically checks file dependencies and runs compiler commands as needed 

## How to Compile

1. Ensure all source files (`oss.c`, `worker.c`, headers, etc.) are in the same directory and that a suitable Makefile is provided.  
2. Run the command:  
   ```
   make
   ```  
   This will compile the code and produce two executable files: **`oss`** (the resource manager) and **`worker`** (the child process).  
3. If compilation succeeds, you can run **`oss`** as the entry point of the simulation. If any errors occur, check that you have C99 support enabled (for example, add `-std=c99` in your Makefile) ([
Status of C99 features in GCC

## How to Run

Use the following command-line syntax to start the simulation:

```
./oss [-h] [-n proc] [-s simul] [-i intervalInMsToLaunchChildren] [-b boundNsForWorkerRequests] [-f logfile]
```

Each flag has the following meaning:

- `-h`  
  Display a help/usage message showing command options.

- `-n proc`  
  Maximum number of worker processes to launch (e.g. `-n 20` allows up to 20 child processes). 

- `-s simul`  
  Total simulated seconds after which the parent stops launching new processes (e.g. `-s 30` stops after 30 simulated seconds). This simulates a timed run of the system.

- `-i intervalInMsToLaunchChildren`  
  Interval in milliseconds between launching child processes (e.g. `-i 250` means **`oss`** spawns a new worker every 250 ms of real time). 

- `-b boundNsForWorkerRequests`  
  Upper bound (in nanoseconds) for how long a worker waits before making another request/release. For example, `-b 100000000` means each worker will wait up to 100 million nanoseconds (0.1 seconds) between its resource operations.

- `-f logfile`  
  Path to the output log file where **`oss`** writes status messages. For example, `-f oss.log`. Only the parent process (`oss`) should write to the log file to avoid concurrent-write issues.

## Example

- **Basic run:**  
  ```
  ./oss -n 10 -s 10 -i 100 -b 50000000 -f oss.log
  ```  
  In this example, up to 10 workers are launched over 10 simulated seconds, with 100 ms between spawns. Each worker waits up to 50 million ns (0.05 s) between requests, and **`oss`** logs to *oss.log*.

- **Help output:**  
  ```
  ./oss -h
  ```  
  Shows usage information and exits.

- **Custom logfile:**  
  ```
  ./oss -n 5 -s 5 -i 200 -b 200000000 -f /tmp/myrun.log
  ```  
  Writes output to `/tmp/myrun.log`.

## Notes

- **Logging:** Only the **`oss`** process writes to the log file. Concurrent writes to a single file by multiple processes are not defined by POSIX and can interleave unpredictably. Having only `oss` write avoids race conditions in the logfile.

- **Deadlock Detection:** The **`oss`** process runs its deadlock detection algorithm once per simulated second (as specified by `-s`). This periodic check ensures that deadlocks are identified promptly

- **Graceful Termination:** On receiving termination signals (`SIGINT` or `SIGTERM`), **`oss`** catches the signal and performs cleanup. It will unlink and close any shared memory segments, message queues, or semaphores before exiting. Using signal handlers for cleanup ensures that shared IPC objects do not remain allocated if the program is killed
