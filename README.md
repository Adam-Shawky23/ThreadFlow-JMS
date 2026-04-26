# ThreadFlow-JMS

> A Unix Job Management System built in C — featuring dynamic process pools, named-pipe IPC, and POSIX signal handling.

---
## Demo

![ThreadFlow-JMS Demo](demo.gif)

## Overview

ThreadFlow-JMS is a multi-process job management system implemented from scratch in C on Linux. It accepts shell commands from a user-facing console, dispatches them through a dynamically-scaling pool architecture, tracks their execution in real time, and captures all output to structured directories.

The project demonstrates low-level Unix systems programming: process creation with `fork()`/`exec()`, inter-process communication via named pipes (`mkfifo`), signal handling with `SIGSTOP`/`SIGCONT`/`SIGTERM`, non-blocking I/O with `O_NONBLOCK`/`fcntl`, and file descriptor management with `dup2`.

---

## Architecture

```
  ┌─────────────────┐
  │   jms_console   │  ← User interface (reads from file or stdin)
  └────────┬────────┘
           │  named pipes: jms_in (commands) / jms_out (responses)
           ▼
  ┌─────────────────┐
  │   jms_coord     │  ← Coordinator (runs permanently, manages all state)
  └────────┬────────┘
           │  named pipes: pool_N_in / pool_N_out (one pair per pool)
     ┌─────┴──────┐
     ▼            ▼
  pool_0        pool_1        ...  (created on demand)
  │    │        │    │
 job  job      job  job            (fork + exec of user commands)
```

### Key design decisions

**Dynamic pool scaling** — Pools are not pre-allocated. The first `submit` creates pool 0. When pool 0 reaches its capacity (`-n` argument), a new pool 1 is created automatically. Each pool is a separate process launched via `fork()` + `execl()`.

**END sentinel protocol** — Every coordinator response ends with `"END\n"`. This allows the console to read responses of arbitrary length without needing a length header or timeout — it simply reads lines until it sees the sentinel.

**Non-blocking pipe polling** — The coordinator cannot block waiting for pool reports while also serving console commands. `process_pool_reports()` temporarily sets `O_NONBLOCK` on each pool's output pipe using `fcntl()`, drains available messages, then restores the original flags. `EAGAIN` is treated as "nothing ready" rather than an error.

**Reconnect-safe console** — When the console disconnects (closes its write end of `jms_in`), reads on that pipe return EOF permanently. The coordinator detects this and re-opens the pipe with `O_NONBLOCK` so it is ready for the next console session without restarting.

**Signal-safe shutdown** — Pool processes install a `SIGTERM` handler that sets only a `volatile sig_atomic_t` flag. All actual cleanup (killing job children, waiting, reporting) happens in normal code when the main loop checks the flag. This avoids calling non-async-signal-safe functions like `printf` or `malloc` inside the handler.

---

## Features

- Submit any shell command as a job: `submit sleep 60`, `submit ls -la /tmp`
- Real-time job status: Active (with elapsed seconds), Finished, Suspended
- Suspend and resume individual jobs with POSIX signals
- Query all active jobs, all finished jobs, all pool processes
- Filter status by time window: `status-all 30` shows only jobs from the last 30 seconds
- Structured output capture: every job's stdout and stderr saved to a timestamped directory
- Automatic pool lifecycle: pools exit cleanly after handling their capacity and report back
- Graceful shutdown: coordinator signals pools, pools terminate their jobs, summary is printed
- Operations file mode: run a batch of commands non-interactively with `-o <file>`
- Bash statistics script: list, size-sort, and purge job output directories

---

## Project Structure

```
ThreadFlow-JMS/
├── jms_common.h      # Shared types (Job, Pool), constants, function prototypes
├── jms_common.c      # Shared utilities: pipe I/O, command parsing, output redirection
├── jms_coord.c       # Coordinator: command dispatch, pool management, job tracking
├── jms_pool.c        # Pool worker: job execution, child reaping, shutdown handling
├── jms_console.c     # User interface: pipe communication, ops file, interactive mode
├── jms_script.sh     # Bash script: list / size / purge job output directories
└── Makefile          # Separate compilation for all three binaries
```

---

## Building

**Requirements:** GCC, GNU Make, Linux (tested on Ubuntu 22.04 and macOS with clang)

```bash
git clone https://github.com/Adam-Shawky23/ThreadFlow-JMS.git
cd ThreadFlow-JMS
make
```

This produces three binaries: `jms_coord`, `jms_console`, `jms_pool`.

```bash
make clean   # remove binaries and object files
```

---

## Usage

### 1. Start the coordinator

Open a terminal and start the coordinator. It runs permanently until you send `shutdown`.

```bash
mkdir -p /tmp/jms_out
./jms_coord -l /tmp/jms_out -n 3
```

| Flag | Description |
|------|-------------|
| `-l <path>` | Directory where job output dirs and pool pipes are created |
| `-n <N>` | Maximum number of jobs each pool process will handle |

### 2. Connect the console

Open a second terminal:

```bash
./jms_console -w jms_in -r jms_out
```

Or with an operations file (runs commands automatically, then falls through to stdin):

```bash
./jms_console -w jms_in -r jms_out -o commands.txt
```

| Flag | Description |
|------|-------------|
| `-w <pipe>` | Named pipe to write commands to the coordinator |
| `-r <pipe>` | Named pipe to read responses from the coordinator |
| `-o <file>` | Optional file of commands to execute before interactive mode |

### 3. Available commands

```
submit <command>        Submit a job for execution
                        Example: submit sleep 60
                        Example: submit ls -la /tmp
                        Returns: JobID: 1, PID: 12345

status <JobID>          Get the status of a specific job
                        Returns: JobID 1 Status: Active (running for 8 seconds)
                                 JobID 1 Status: Finished
                                 JobID 1 Status: Suspended

status-all [n]          Get the status of all jobs
                        Optional: only jobs submitted in the last n seconds
                        Returns one status line per job

show-active             List all currently running jobs
show-finished           List all completed jobs
show-pools              List all pool processes with their PID and active job count

suspend <JobID>         Pause a job (sends SIGSTOP)
                        Returns: Sent suspend signal to JobID 1

resume <JobID>          Resume a paused job (sends SIGCONT)
                        Returns: Sent resume signal to JobID 1

shutdown                Gracefully terminate the entire system
                        Returns: Served 5 jobs, 2 were still in progress
```

### 4. Example session

```
jms> submit sleep 60
JobID: 1, PID: 4521

jms> submit echo hello_world
JobID: 2, PID: 4528

jms> submit sleep 60
JobID: 3, PID: 4535

jms> show-pools
Pool & NumOfJobs:
4520 2
4534 1

jms> status 2
JobID 2 Status: Finished

jms> show-active
Active jobs:
JobID 1
JobID 3

jms> suspend 1
Sent suspend signal to JobID 1

jms> status 1
JobID 1 Status: Suspended

jms> resume 1
Sent resume signal to JobID 1

jms> shutdown
Served 3 jobs, 2 were still in progress
```

### 5. Statistics script

```bash
# List all job output directories
./jms_script.sh -l /tmp/jms_out -c list

# List directories sorted by size (ascending)
./jms_script.sh -l /tmp/jms_out -c size

# Show only the 3 largest
./jms_script.sh -l /tmp/jms_out -c "size 3"

# Delete all output directories
./jms_script.sh -l /tmp/jms_out -c purge
```

Flags can be passed in any order: `-c list -l /tmp/jms_out` works the same as `-l /tmp/jms_out -c list`.

---

## Output Directory Format

Each job automatically creates a directory:

```
/tmp/jms_out/outputs_<jobid>_<pid>_<YYYYMMDD>_<HHMMSS>/
    stdout_<jobid>     # Standard output of the job
    stderr_<jobid>     # Standard error of the job
```

Example:
```
/tmp/jms_out/outputs_2_4528_20260412_143022/
    stdout_2           # contains "hello_world"
    stderr_2           # empty (echo produces no stderr)
```

Output redirection is done with `dup2()` in the child process after `fork()` and before `execvp()`, so the job itself writes normally to stdout/stderr without any awareness of the redirection.

---

## IPC Protocol Details

### Console ↔ Coordinator

```
Console  →  Coordinator:   "submit sleep 60\n"
Coordinator  →  Console:   "JobID: 1, PID: 4521\n"
                            "END\n"
```

Every coordinator response ends with `"END\n"`. The console reads lines until it sees this sentinel, enabling variable-length responses without length headers.

### Coordinator ↔ Pool

```
Coordinator → Pool:   "run 1 sleep 60\n"
Pool → Coordinator:   "started 1 4521\n"
Pool → Coordinator:   "finished 1 4521\n"   (when job exits)
Pool → Coordinator:   "done\n"              (pool handled all capacity, exiting)
```

Pool pipes are stored in the base directory as `pool_N_in` and `pool_N_out`. On shutdown, `SIGTERM` replaces the `"run"` message — the pool catches it, terminates its jobs, sends `"finished"` for each, then sends `"done"`.

---

## Technical Highlights

| Concept | Implementation |
|---------|---------------|
| Process creation | `fork()` + `execl()` for pools, `fork()` + `execvp()` for jobs |
| IPC | Named pipes (`mkfifo`) with custom line-based protocol |
| Non-blocking I/O | `fcntl(fd, F_SETFL, flags \| O_NONBLOCK)` for pool pipe polling |
| Output capture | `dup2(fd, STDOUT_FILENO)` before `exec` in job child |
| Job suspension | `kill(pid, SIGSTOP)` / `kill(pid, SIGCONT)` |
| Graceful shutdown | `SIGTERM` → pool signal handler sets flag → cleanup in main loop |
| Zombie prevention | `waitpid(pid, NULL, WNOHANG)` polled on every command |
| Reconnect handling | Coordinator re-opens `jms_in` with `O_NONBLOCK` on EOF |

---

## License

MIT License — see [LICENSE](LICENSE) for details.