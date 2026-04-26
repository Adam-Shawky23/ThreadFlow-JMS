#include "jms_common.h"

//Global state
static Job  jobs[MAX_JOBS];
static int  job_count   = 0;
static int  next_job_id = 0;

static Pool pools[MAX_POOLS];
static int  pool_count  = 0;

static char base_path[MAX_PATH_LEN];
static int  jobs_pool_size = 0;

static int  fd_console_in  = -1;
static int  fd_console_out = -1;

//Forward Declarations of the functions
static void refresh_job_statuses(void);
static int  find_job_index(int job_id);
static int  get_or_create_pool(void);
static void process_pool_reports(void);
static void handle_submit(const char *job_cmd);
static void handle_status(int job_id);
static void handle_status_all(const char *arg);
static void handle_show_active(void);
static void handle_show_pools(void);
static void handle_show_finished(void);
static void handle_suspend(int job_id);
static void handle_resume(int job_id);
static void handle_shutdown(void);

/* ─────────────────────────────────────────────
 * refresh_job_statuses()
 *
 * For jobs run by pools, the pool reports "finished" messages
 * which process_pool_reports() handles. But we also call this
 * to catch any edge cases where the pool process itself died
 * without sending a report.
 * ───────────────────────────────────────────── */
static void refresh_job_statuses(void) {
    process_pool_reports();
}

/* ─────────────────────────────────────────────
 * find_job_index()
 * ───────────────────────────────────────────── */
static int find_job_index(int job_id) {
    for (int i = 0; i < job_count; i++)
        if (jobs[i].job_id == job_id) return i;
    return -1;
}

/* ─────────────────────────────────────────────
 * process_pool_reports()
 *
 * Drains pending messages from all active pool pipes
 * using non-blocking reads. Updates job statuses based
 * on what the pools report.
 *
 * Messages:
 *   "started <jobid> <pid>"  -> update job's real PID
 *   "finished <jobid> <pid>" -> mark job FINISHED
 *   "done"                   -> pool is exiting, reap it
 * ───────────────────────────────────────────── */

static void process_pool_reports(void) {
    for (int i = 0; i < pool_count; i++) {
        if (!pools[i].active) continue;

        int flags = fcntl(pools[i].fd_out, F_GETFL);
        fcntl(pools[i].fd_out, F_SETFL, flags | O_NONBLOCK);

        char line[BUF_SIZE];
        while (pipe_read_line(pools[i].fd_out, line, sizeof(line)) > 0) {
            line[strcspn(line, "\n")] = '\0';

            if (strncmp(line, "started ", 8) == 0) {
                int job_id, pid;
                if (sscanf(line, "started %d %d", &job_id, &pid) == 2) {
                    int idx = find_job_index(job_id);
                    if (idx >= 0) jobs[idx].pid = (pid_t)pid;
                }
            } else if (strncmp(line, "finished ", 9) == 0) {
                int job_id, pid;
                if (sscanf(line, "finished %d %d", &job_id, &pid) == 2) {
                    int idx = find_job_index(job_id);
                    if (idx >= 0) jobs[idx].status = JOB_FINISHED;
                }
            } else if (strcmp(line, "done") == 0) {
                pools[i].active = 0;
                waitpid(pools[i].pid, NULL, 0);
                close(pools[i].fd_in);
                close(pools[i].fd_out);
            }
        }

        fcntl(pools[i].fd_out, F_SETFL, flags);
    }
}

/* ─────────────────────────────────────────────
 * get_or_create_pool()
 *
 * Finds a pool with remaining capacity or creates a new one.
 * Returns pool index, or -1 on failure.
 *
 * Each pool gets its own pair of named pipes:
 *   <base_path>/pool_<idx>_in   coord writes job commands here
 *   <base_path>/pool_<idx>_out  coord reads pool reports here
 * ───────────────────────────────────────────── */

static int get_or_create_pool(void) {
    // Look for an existing pool with room 
    for (int i = 0; i < pool_count; i++) {
        if (pools[i].active &&
            pools[i].jobs_assigned < pools[i].jobs_capacity)
            return i;
    }

    if (pool_count >= MAX_POOLS) return -1;

    int  idx = pool_count;
    Pool *p  = &pools[idx];

    // Build unique pipe paths 
    snprintf(p->pipe_in_path,  MAX_PATH_LEN - 20,
             "%s/pool_%d_in",  base_path, idx);
    snprintf(p->pipe_out_path, MAX_PATH_LEN - 20,
             "%s/pool_%d_out", base_path, idx);

    unlink(p->pipe_in_path);
    unlink(p->pipe_out_path);

    if (mkfifo(p->pipe_in_path,  0666) < 0 ||
        mkfifo(p->pipe_out_path, 0666) < 0) {
        perror("get_or_create_pool: mkfifo");
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) { perror("get_or_create_pool: fork"); return -1; }

    if (pid == 0) {
        // CHILD becomes jms_pool 
        close(fd_console_in);
        close(fd_console_out);

        char cap_str[16];
        snprintf(cap_str, sizeof(cap_str), "%d", jobs_pool_size);

        execl("./jms_pool", "jms_pool",
              "-i", p->pipe_in_path,
              "-o", p->pipe_out_path,
              "-n", cap_str,
              "-l", base_path,
              NULL);
        perror("get_or_create_pool: execl jms_pool");
        exit(EXIT_FAILURE);
    }

    /* PARENT: open coord side of pool pipes 
      O_RDWR on both avoids blocking while the pool
      process is still starting up. */
    p->fd_in  = open(p->pipe_in_path,  O_RDWR);
    p->fd_out = open(p->pipe_out_path, O_RDWR);

    if (p->fd_in < 0 || p->fd_out < 0) {
        perror("get_or_create_pool: open ");
        return -1;
    }

    p->pid           = pid;
    p->jobs_assigned = 0;
    p->jobs_capacity = jobs_pool_size;
    p->active        = 1;
    pool_count++;

    return idx;
}

/* ─────────────────────────────────────────────
 * handle_submit()
 *
 * Delegates job execution to a pool process.
 *
 * 1. Find/create a pool
 * 2. Send "run <jobid> <command>" to pool's pipe
 * 3. Read back "started <jobid> <pid>" for the real PID
 * 4. Record job, respond to console
 * ───────────────────────────────────────────── */
static void handle_submit(const char *job_cmd) {
    if (job_count >= MAX_JOBS) {
        pipe_write_str(fd_console_out,
                       "Error: max jobs reached\n" END_SENTINEL);
        return;
    }

    process_pool_reports();

    int pool_idx = get_or_create_pool();
    if (pool_idx < 0) {
        pipe_write_str(fd_console_out,
                       "Error: could not create pool\n" END_SENTINEL);
        return;
    }

    int job_id = ++next_job_id;

    // Send run command to pool 
    char run_msg[BUF_SIZE];
    snprintf(run_msg, sizeof(run_msg), "run %d %s\n", job_id, job_cmd);
    pipe_write_str(pools[pool_idx].fd_in, run_msg);

    // Record job with PID=0 as placeholder until "started" reply 
    jobs[job_count].job_id     = job_id;
    jobs[job_count].pid        = 0;
    jobs[job_count].status     = JOB_ACTIVE;
    jobs[job_count].start_time = time(NULL);
    jobs[job_count].pool_idx   = pool_idx;
    strncpy(jobs[job_count].cmd, job_cmd, MAX_CMD_LEN - 1);
    job_count++;
    pools[pool_idx].jobs_assigned++;

    // Read "started" reply to get the real PID.
    //Temporarily block on this one read. 
    int save_flags = fcntl(pools[pool_idx].fd_out, F_GETFL);
    fcntl(pools[pool_idx].fd_out, F_SETFL,
          save_flags & ~O_NONBLOCK);

    char reply[BUF_SIZE];
    if (pipe_read_line(pools[pool_idx].fd_out, reply, sizeof(reply)) > 0) {
        reply[strcspn(reply, "\n")] = '\0';
        int job_id_got, pid_got;
        if (sscanf(reply, "started %d %d", &job_id_got, &pid_got) == 2) {
            int idx = find_job_index(job_id_got);
            if (idx >= 0) jobs[idx].pid = (pid_t)pid_got;
        }
    }

    fcntl(pools[pool_idx].fd_out, F_SETFL, save_flags);

    int final_pid = (int)jobs[job_count - 1].pid;
    char msg[BUF_SIZE];
    snprintf(msg, sizeof(msg),
             "JobID: %d, PID: %d\n" END_SENTINEL, job_id, final_pid);
    pipe_write_str(fd_console_out, msg);
}

/* ─────────────────────────────────────────────
 * handle_status()
 *
 * We poll in a tight loop — drain reports, check if job transitioned to FINISHED, sleep
 * briefly, repeat — for up to 500ms before giving up and reporting
 * whatever status we currently have. This makes "status" accurate
 * without blocking the coordinator for long, needed for function like echo which finishes really fast.
 * ───────────────────────────────────────────── */
static void handle_status(int job_id) {
    // First drain whatever is already buffered 
    process_pool_reports();

    //Check if started
    int idx_check = find_job_index(job_id);
    if (idx_check >= 0 && jobs[idx_check].status == JOB_ACTIVE) {
        // Poll up to 10 times, 50ms apart = 500ms max wait 
        int polls = 0;
        while (polls < 10) {
            usleep(50000); // 50ms 
            process_pool_reports();
            int idx2 = find_job_index(job_id);
            if (idx2 < 0 || jobs[idx2].status != JOB_ACTIVE) break;
            polls++;
        }
    }

    char msg[BUF_SIZE];
    int  idx = find_job_index(job_id);

    if (idx < 0) {
        snprintf(msg, sizeof(msg),
                 "JobID %d: not found\n" END_SENTINEL, job_id);
        pipe_write_str(fd_console_out, msg);
        return;
    }

    Job *j = &jobs[idx];
    switch (j->status) {
        case JOB_ACTIVE: {
            long elapsed = (long)(time(NULL) - j->start_time);
            snprintf(msg, sizeof(msg),
                "JobID %d Status: Active (running for %ld seconds)\n"
                END_SENTINEL, job_id, elapsed);
            break;
        }
        case JOB_FINISHED:
            snprintf(msg, sizeof(msg),
                "JobID %d Status: Finished\n" END_SENTINEL, job_id);
            break;
        case JOB_SUSPENDED:
            snprintf(msg, sizeof(msg),
                "JobID %d Status: Suspended\n" END_SENTINEL, job_id);
            break;
        default:
            snprintf(msg, sizeof(msg),
                "JobID %d Status: Unknown\n" END_SENTINEL, job_id);
    }
    pipe_write_str(fd_console_out, msg);
}

/* ─────────────────────────────────────────────
 * handle_status_all()
 *
 * Same Logic as handle_status() but does it for all
 * ───────────────────────────────────────────── */
static void handle_status_all(const char *arg) {
    refresh_job_statuses();

    int time_window = -1;
    if (arg && strlen(arg) > 0) time_window = atoi(arg);

    time_t now = time(NULL);
    char   msg[BUF_SIZE * 8];
    int    pos = 0;

    if (job_count == 0) {
        pipe_write_str(fd_console_out,
                       "No jobs submitted yet\n" END_SENTINEL);
        return;
    }

    for (int i = 0; i < job_count; i++) {
        Job *j = &jobs[i];
        if (time_window >= 0 && (long)(now - j->start_time) > time_window)
            continue;

        int w = 0;
        switch (j->status) {
            case JOB_ACTIVE:
                w = snprintf(msg+pos, sizeof(msg)-pos,
                    "JobID %d Status: Active (running for %ld sec)\n",
                    j->job_id, (long)(now - j->start_time));
                break;
            case JOB_FINISHED:
                w = snprintf(msg+pos, sizeof(msg)-pos,
                    "JobID %d Status: Finished\n", j->job_id);
                break;
            case JOB_SUSPENDED:
                w = snprintf(msg+pos, sizeof(msg)-pos,
                    "JobID %d Status: Suspended\n", j->job_id);
                break;
            default: break;
        }
        if (w > 0) pos += w;
    }

    if (pos == 0) {
        pipe_write_str(fd_console_out,
                       "No jobs in the specified time window\n" END_SENTINEL);
        return;
    }
    snprintf(msg+pos, sizeof(msg)-pos, END_SENTINEL);
    pipe_write_str(fd_console_out, msg);
}

/* ─────────────────────────────────────────────
 * handle_show_active()
 *
 * Also same logic but show to console all that are active
 * ───────────────────────────────────────────── */
static void handle_show_active(void) {
    refresh_job_statuses();
    char msg[BUF_SIZE * 8];
    int  pos = 0;

    pos += snprintf(msg+pos, sizeof(msg)-pos, "Active jobs:\n");
    int found = 0;
    for (int i = 0; i < job_count; i++) {
        if (jobs[i].status == JOB_ACTIVE) {
            pos += snprintf(msg+pos, sizeof(msg)-pos,
                            "JobID %d\n", jobs[i].job_id);
            found++;
        }
    }
    if (!found) pos += snprintf(msg+pos, sizeof(msg)-pos, "(none)\n");
    snprintf(msg+pos, sizeof(msg)-pos, END_SENTINEL);
    pipe_write_str(fd_console_out, msg);
}

/* ─────────────────────────────────────────────
 * handle_show_pools()
 * 
 * Shows all the pools that are created
 * ───────────────────────────────────────────── */
static void handle_show_pools(void) {
    process_pool_reports();
    char msg[BUF_SIZE * 4];
    int  pos = 0;

    pos += snprintf(msg+pos, sizeof(msg)-pos, "Pool & NumOfJobs:\n");
    int found = 0;
    for (int i = 0; i < pool_count; i++) {
        if (!pools[i].active) continue;
        int njobs = 0;
        for (int j = 0; j < job_count; j++)
            if (jobs[j].pool_idx == i && jobs[j].status == JOB_ACTIVE)
                njobs++;
        pos += snprintf(msg+pos, sizeof(msg)-pos,
                        "%d %d\n", (int)pools[i].pid, njobs);
        found++;
    }
    if (!found) pos += snprintf(msg+pos, sizeof(msg)-pos,
                                "(no active pools)\n");
    snprintf(msg+pos, sizeof(msg)-pos, END_SENTINEL);
    pipe_write_str(fd_console_out, msg);
}

/* ─────────────────────────────────────────────
 * handle_show_finished()
 *
 * Same as with show active but instead shows the processes that have finished
 * ───────────────────────────────────────────── */
static void handle_show_finished(void) {
    refresh_job_statuses();
    char msg[BUF_SIZE * 8];
    int  pos = 0;

    pos += snprintf(msg+pos, sizeof(msg)-pos, "Finished jobs:\n");
    int found = 0;
    for (int i = 0; i < job_count; i++) {
        if (jobs[i].status == JOB_FINISHED) {
            pos += snprintf(msg+pos, sizeof(msg)-pos,
                            "JobID %d\n", jobs[i].job_id);
            found++;
        }
    }
    if (!found) pos += snprintf(msg+pos, sizeof(msg)-pos, "(none)\n");
    snprintf(msg+pos, sizeof(msg)-pos, END_SENTINEL);
    pipe_write_str(fd_console_out, msg);
}

/* ─────────────────────────────────────────────
 * handle_suspend()
 *
 * gives the ability to suspend a process (pause it) and then you can resume it.
 * ───────────────────────────────────────────── */
static void handle_suspend(int job_id) {
    char msg[BUF_SIZE];
    int  idx = find_job_index(job_id);

    if (idx < 0) {
        snprintf(msg, sizeof(msg),
                 "JobID %d: not found\n" END_SENTINEL, job_id);
        pipe_write_str(fd_console_out, msg);
        return;
    }
    if (jobs[idx].status == JOB_FINISHED) {
        snprintf(msg, sizeof(msg),
                 "JobID %d is not active\n" END_SENTINEL, job_id);
        pipe_write_str(fd_console_out, msg);
        return;
    }
    if (jobs[idx].status == JOB_SUSPENDED) {
        snprintf(msg, sizeof(msg),
                 "JobID %d is already suspended\n" END_SENTINEL, job_id);
        pipe_write_str(fd_console_out, msg);
        return;
    }
    if (kill(jobs[idx].pid, SIGSTOP) < 0) {
        jobs[idx].status = JOB_FINISHED;
        snprintf(msg, sizeof(msg),
                 "JobID %d already finished\n" END_SENTINEL, job_id);
        pipe_write_str(fd_console_out, msg);
        return;
    }
    jobs[idx].status = JOB_SUSPENDED;
    snprintf(msg, sizeof(msg),
             "Sent suspend signal to JobID %d\n" END_SENTINEL, job_id);
    pipe_write_str(fd_console_out, msg);
}

/* ─────────────────────────────────────────────
 * handle_resume()
 *
 * Resumes a process only if it was suspended before hand
 * ───────────────────────────────────────────── */
static void handle_resume(int job_id) {
    char msg[BUF_SIZE];
    int  idx = find_job_index(job_id);

    if (idx < 0) {
        snprintf(msg, sizeof(msg),
                 "JobID %d: not found\n" END_SENTINEL, job_id);
        pipe_write_str(fd_console_out, msg);
        return;
    }
    if (jobs[idx].status != JOB_SUSPENDED) {
        snprintf(msg, sizeof(msg),
                 "JobID %d is not suspended\n" END_SENTINEL, job_id);
        pipe_write_str(fd_console_out, msg);
        return;
    }
    if (kill(jobs[idx].pid, SIGCONT) < 0) {
        jobs[idx].status = JOB_FINISHED;
        snprintf(msg, sizeof(msg),
                 "JobID %d already finished\n" END_SENTINEL, job_id);
        pipe_write_str(fd_console_out, msg);
        return;
    }
    jobs[idx].status = JOB_ACTIVE;
    snprintf(msg, sizeof(msg),
             "Sent resume signal to JobID %d\n" END_SENTINEL, job_id);
    pipe_write_str(fd_console_out, msg);
}

/* ─────────────────────────────────────────────
 * handle_shutdown()
 *
 * Uses non-blocking poll loop to avoid deadlock.
 * After SIGTERM is sent to pools, we poll their output
 * pipes until we see "done" OR the pool process exits.
 * ───────────────────────────────────────────── */
static void handle_shutdown(void) {
    process_pool_reports();

    // Count in-progress jobs BEFORE signalling 
    int still_running = 0;
    for (int i = 0; i < job_count; i++)
        if (jobs[i].status == JOB_ACTIVE ||
            jobs[i].status == JOB_SUSPENDED)
            still_running++;

    // Signal all active pools 
    for (int i = 0; i < pool_count; i++)
        if (pools[i].active)
            kill(pools[i].pid, SIGTERM);

    /* Give pools a moment to start their shutdown sequence
     * before we begin polling — avoids spinning on empty pipes */
    usleep(100000); //100ms

    // Poll until every pool has exited 
    for (int i = 0; i < pool_count; i++) {
        if (!pools[i].active) continue;

        // Non-blocking so we can also check waitpid 
        int flags = fcntl(pools[i].fd_out, F_GETFL);
        fcntl(pools[i].fd_out, F_SETFL, flags | O_NONBLOCK);

        int pool_done = 0;
        int attempts  = 0;
        while (!pool_done && attempts < 200) { /* 200 * 20ms = 4s max */
            attempts++;
            char line[BUF_SIZE];
            while (pipe_read_line(pools[i].fd_out,
                                  line, sizeof(line)) > 0) {
                line[strcspn(line, "\r\n")] = '\0';
                if (strlen(line) == 0) continue;

                if (strncmp(line, "finished ", 9) == 0) {
                    int job_id, pid;
                    if (sscanf(line, "finished %d %d",
                               &job_id, &pid) == 2) {
                        int idx = find_job_index(job_id);
                        if (idx >= 0)
                            jobs[idx].status = JOB_FINISHED;
                    }
                } else if (strcmp(line, "done") == 0) {
                    pool_done = 1;
                    break;
                }
            }
            if (pool_done) break;

            // Pool may have exited without us seeing "done" 
            int wstatus;
            pid_t r = waitpid(pools[i].pid, &wstatus, WNOHANG);
            if (r > 0 || (r < 0 && errno == ECHILD)) {
                pool_done = 1;
                break;
            }

            usleep(20000); // 20ms poll interval 
        }

        waitpid(pools[i].pid, NULL, WNOHANG);
        close(pools[i].fd_in);
        close(pools[i].fd_out);
        pools[i].active = 0;
    }

    char msg[BUF_SIZE];
    snprintf(msg, sizeof(msg),
             "Served %d jobs, %d were still in progress\n" END_SENTINEL,
             job_count, still_running);
    pipe_write_str(fd_console_out, msg);
}

/* ─────────────────────────────────────────────
 * setup_named_pipes()
 *
 * A function to set up the pipes so that we can work with them
 * ───────────────────────────────────────────── */
static void setup_named_pipes(void) {
    unlink(PIPE_IN);
    unlink(PIPE_OUT);
    if (mkfifo(PIPE_IN, 0666) < 0) {
        perror("jms_coord: mkfifo jms_in"); exit(EXIT_FAILURE);
    }
    if (mkfifo(PIPE_OUT, 0666) < 0) {
        perror("jms_coord: mkfifo jms_out"); exit(EXIT_FAILURE);
    }
}

/* ─────────────────────────────────────────────
 * open_named_pipes()
 *
 * Function to actually open the pipes so that we can start to use them
 * ───────────────────────────────────────────── */
static void open_named_pipes(void) {
    fd_console_in = open(PIPE_IN, O_RDONLY | O_NONBLOCK);
    if (fd_console_in < 0) {
        perror("jms_coord: open jms_in"); exit(EXIT_FAILURE);
    }
    int flags = fcntl(fd_console_in, F_GETFL);
    fcntl(fd_console_in, F_SETFL, flags & ~O_NONBLOCK);

    fd_console_out = open(PIPE_OUT, O_RDWR);
    if (fd_console_out < 0) {
        perror("jms_coord: open jms_out"); exit(EXIT_FAILURE);
    }

}

/* ─────────────────────────────────────────────
 * dispatch_command()
 *
 * Connects everything that the user write in console to the corresponding function
 * ───────────────────────────────────────────── */
static int dispatch_command(char *line) {
    line[strcspn(line, "\n")] = '\0';

    if      (strncmp(line, "submit ", 7) == 0)
        handle_submit(line + 7);
    else if (strncmp(line, "status-all", 10) == 0)
        handle_status_all((strlen(line) > 11) ? line + 11 : NULL);
    else if (strncmp(line, "status ", 7) == 0)
        handle_status(atoi(line + 7));
    else if (strcmp(line, "show-active") == 0)
        handle_show_active();
    else if (strcmp(line, "show-pools") == 0)
        handle_show_pools();
    else if (strcmp(line, "show-finished") == 0)
        handle_show_finished();
    else if (strncmp(line, "suspend ", 8) == 0)
        handle_suspend(atoi(line + 8));
    else if (strncmp(line, "resume ", 7) == 0)
        handle_resume(atoi(line + 7));
    else if (strcmp(line, "shutdown") == 0) {
        handle_shutdown();
        return 1;
    } else {
        char msg[BUF_SIZE];
        snprintf(msg, sizeof(msg),
                 "Unknown command: %s\n" END_SENTINEL, line);
        pipe_write_str(fd_console_out, msg);
    }
    return 0;
}

/* ─────────────────────────────────────────────
 * main()
 * ───────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    int opt;
    while ((opt = getopt(argc, argv, "l:n:")) != -1) {
        switch (opt) {
            case 'l': strncpy(base_path, optarg, MAX_PATH_LEN-1); break;
            case 'n': jobs_pool_size = atoi(optarg); break;
            default:
                exit(EXIT_FAILURE);
        }
    }

    if (!strlen(base_path) || jobs_pool_size <= 0) {
        exit(EXIT_FAILURE);
    }

    mkdir(base_path, 0755);

    setup_named_pipes();
    open_named_pipes();

    char line[BUF_SIZE];
    while (1) {
        int n = pipe_read_line(fd_console_in, line, sizeof(line));

        if (n <= 0) {
            /* EOF — the console disconnected (closed its write end).
             * Re-open jms_in so we are ready for the next console session.
             * We must close and re-open because once a pipe's write end is
             * closed, reads return EOF forever until reopened. */
            close(fd_console_in);
            fd_console_in = open(PIPE_IN, O_RDONLY | O_NONBLOCK);
            if (fd_console_in >= 0) {
                int fl = fcntl(fd_console_in, F_GETFL);
                fcntl(fd_console_in, F_SETFL, fl & ~O_NONBLOCK);
            }
            usleep(100000);
            continue;
        }

        if (line[0] == '\n' || line[0] == '\0') continue;
        if (dispatch_command(line)) break;
    }

    close(fd_console_in);
    close(fd_console_out);
    unlink(PIPE_IN);
    unlink(PIPE_OUT);
    return 0;
}