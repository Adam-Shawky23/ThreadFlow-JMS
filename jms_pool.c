#include "jms_common.h"

//Pool per job record
typedef struct {
    int   job_id;
    pid_t pid;
    int   running; /* 1 = still running, 0 = finished */
} PoolJob;

//Global State so that SIGTERM can acess them
static int      fd_in      = -1;  // read job commands from coord  
static int      fd_out     = -1;  // write reports back to coord   
static int      capacity   = 0;   // max total jobs we will accept 
static char     base_path[MAX_PATH_LEN] = "";

static PoolJob  my_jobs[1024];    // jobs running in this pool     
static int      my_job_count = 0;

// Set to 1 by the SIGTERM handler — checked in the main loop 
static volatile sig_atomic_t got_sigterm = 0;

/* ─────────────────────────────────────────────
 * sigterm_handler()
 *
 * When jms_coord sends SIGTERM (on shutdown), we set a flag.
 * We do NOT do complex work inside the signal handler itself —
 * signal handlers should be as simple as possible because they
 * can interrupt the program at any point and cause subtle bugs
 * if they call non-reentrant functions like printf() or malloc().
 *
 * We just set a flag here and let the main loop notice it.
 * ───────────────────────────────────────────── */
static void sigterm_handler(int sig) {
    (void)sig;          //surpress warning
    got_sigterm = 1;
}

/* ─────────────────────────────────────────────
 * reap_finished_jobs()
 *
 * Checks all our running jobs with WNOHANG.
 * For each one that finished, sends "finished <jobid> <pid>\n"
 * back to the coordinator so it can update its job table.
 * ───────────────────────────────────────────── */
static void reap_finished_jobs(void) {
    for (int i = 0; i < my_job_count; i++) {
        if (!my_jobs[i].running) continue;

        int wstatus;
        pid_t result = waitpid(my_jobs[i].pid, &wstatus, WNOHANG);

        if (result > 0) {
            // Child finished — report to coord 
            my_jobs[i].running = 0;

            char msg[BUF_SIZE];
            snprintf(msg, sizeof(msg), "finished %d %d\n",
                     my_jobs[i].job_id, (int)my_jobs[i].pid);
            pipe_write_str(fd_out, msg);
        }
    }
}

/* ─────────────────────────────────────────────
 * run_job()
 *
 * Forks and execs a single job command.
 * Creates the output directory and redirects stdout/stderr.
 * Reports "started <jobid> <pid>" back to coord.
 * Returns the child PID, or -1 on failure.
 * ───────────────────────────────────────────── */
static pid_t run_job(int job_id, const char *cmd) {
    pid_t pid = fork();

    if (pid < 0) {
        perror("jms_pool: fork");
        return -1;
    }

    if (pid == 0) {
        // CHILD (the actual job)
        // Close the pool's pipe FDs
        close(fd_in);
        close(fd_out);

        // Create output directory and redirect I/O 
        char out_dir[MAX_PATH_LEN];
        if (create_output_dir(job_id, getpid(), base_path, out_dir) < 0)
            exit(1);
        if (redirect_output(job_id, out_dir) < 0)
            exit(1);

        // Parse and exec the command 
        int argc_val;
        char **argv = parse_command(cmd, &argc_val);
        if (!argv) exit(1);

        execvp(argv[0], argv);
        perror("jms_pool: execvp");
        exit(127);
    }

    // PARENT (pool continues) 

    // Record this job 
    my_jobs[my_job_count].job_id  = job_id;
    my_jobs[my_job_count].pid     = pid;
    my_jobs[my_job_count].running = 1;
    my_job_count++;

    // Report the new PID back to coord immediately 
    char msg[BUF_SIZE];
    snprintf(msg, sizeof(msg), "started %d %d\n", job_id, (int)pid);
    pipe_write_str(fd_out, msg);

    return pid;
}

/* ─────────────────────────────────────────────
 * shutdown_pool()
 *
 * Called when SIGTERM is received.
 * Sends SIGTERM to all running jobs, waits for them,
 * reports all unfinished jobs as finished, then sends "done".
 * ───────────────────────────────────────────── */
static void shutdown_pool(void) {
    // Signal all still-running job children 
    for (int i = 0; i < my_job_count; i++) {
        if (my_jobs[i].running) {
            kill(my_jobs[i].pid, SIGTERM);
        }
    }

    // Wait for all children and report any that hadn't finished yet 
    for (int i = 0; i < my_job_count; i++) {
        if (my_jobs[i].running) {
            waitpid(my_jobs[i].pid, NULL, 0);

            // Report as finished to coord 
            char msg[BUF_SIZE];
            snprintf(msg, sizeof(msg), "finished %d %d\n",
                     my_jobs[i].job_id, (int)my_jobs[i].pid);
            pipe_write_str(fd_out, msg);
            my_jobs[i].running = 0;
        }
    }

    // Tell coord this pool is done 
    pipe_write_str(fd_out, "done\n");
}

/* ─────────────────────────────────────────────
 * main()
 * ───────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    char pipe_in_path[MAX_PATH_LEN]  = "";
    char pipe_out_path[MAX_PATH_LEN] = "";
    int  opt;

    //  Parse arguments passed by jms_coord 
    while ((opt = getopt(argc, argv, "i:o:n:l:")) != -1) {
        switch (opt) {
            case 'i': strncpy(pipe_in_path,  optarg, MAX_PATH_LEN-1); break;
            case 'o': strncpy(pipe_out_path, optarg, MAX_PATH_LEN-1); break;
            case 'n': capacity = atoi(optarg); break;
            case 'l': strncpy(base_path,     optarg, MAX_PATH_LEN-1); break;
            default:
                exit(EXIT_FAILURE);
        }
    }

    if (!strlen(pipe_in_path) || !strlen(pipe_out_path) ||
        capacity <= 0         || !strlen(base_path)) {
        exit(EXIT_FAILURE);
    }

    /* Install SIGTERM handler BEFORE opening pipes.
     * If coord sends SIGTERM before we even start the main loop,
     * we need to be ready to handle it. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa, NULL);

    /* Open our dedicated named pipes.
     * These were created by jms_coord before it exec'd us. */
    fd_in  = open(pipe_in_path,  O_RDONLY);
    fd_out = open(pipe_out_path, O_RDWR);

    if (fd_in < 0 || fd_out < 0) {
        perror("jms_pool: open pipes");
        exit(EXIT_FAILURE);
    }

    /* ── Main job loop ──
     *
     * We accept exactly `capacity` jobs total, one at a time.
     * Between reading jobs, we also reap any children that finished
     * (calling reap_finished_jobs()) so we always report promptly.
     *
     * The loop also checks got_sigterm after every read so that
     * a shutdown signal is never ignored for more than one iteration.
     */
    int jobs_accepted = 0;

    while (jobs_accepted < capacity) {

        // Check for SIGTERM before blocking on read 
        if (got_sigterm) {
            shutdown_pool();
            goto cleanup;
        }

        /*Poll for the next "run" command using non-blocking reads.
          This allows reap_finished_jobs() to run while waiting,
          which is critical for fast jobs (e.g. echo) that finis */
        char line[BUF_SIZE];
        int  got_job = 0;
 
        // Set fd_in to non-blocking for the poll loop 
        int in_flags = fcntl(fd_in, F_GETFL);
        fcntl(fd_in, F_SETFL, in_flags | O_NONBLOCK);
 
        while (!got_job && !got_sigterm) {
            int n = pipe_read_line(fd_in, line, sizeof(line));
 
            if (n > 0) {
                // Got a message 
                got_job = 1;
            } else {
                // Nothing ready yet — reap any finished children
                reap_finished_jobs();
                usleep(20000); // 20ms poll interval 
            }
        }
 
        // Restore blocking mode 
        fcntl(fd_in, F_SETFL, in_flags);
 
        if (got_sigterm) { shutdown_pool(); goto cleanup; }
 
        // Strip newline and parse "run <jobid> <command...>" 
        line[strcspn(line, "\n")] = '\0';
 
        int job_id;
        char cmd[MAX_CMD_LEN];
        if (sscanf(line, "run %d %1023[^\n]", &job_id, cmd) != 2)
            continue; //skip
 
        // Launch the job 
        run_job(job_id, cmd);
        jobs_accepted++;
 
        // Reap any children that finished during or before this job 
        reap_finished_jobs();
    }

    /* We've accepted all the jobs we're allowed.
     * Now wait for any still-running children to finish. */
    while (1) {
        if (got_sigterm) {
            shutdown_pool();
            goto cleanup;
        }

        // Check if any children are still running 
        int still_running = 0;
        for (int i = 0; i < my_job_count; i++) {
            if (my_jobs[i].running) { still_running = 1; break; }
        }
        if (!still_running) break;

        // Sleep briefly then reap — avoids busy-spinning 
        usleep(50000); // 50ms
        reap_finished_jobs();
    }

    // All jobs done normally — tell coord we're finished 
    pipe_write_str(fd_out, "done\n");

cleanup:
    close(fd_in);
    close(fd_out);
    return 0;
}