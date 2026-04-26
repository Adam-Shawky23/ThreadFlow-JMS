#include "jms_common.h"

/* ─────────────────────────────────────────────
 * pipe_write_str()
 *
 * Why we loop: write() can return fewer bytes than requested if interrupted
 * by a signal or if the pipe buffer is temporarily full. We must keep writing
 * until all bytes are sent.
 * ───────────────────────────────────────────── */

int pipe_write_str(int fd, const char *msg) {
    size_t total  = strlen(msg);
    size_t written = 0;

    while (written < total) {
        ssize_t n = write(fd, msg + written, total - written);
        if (n < 0) {
            if (errno == EINTR) continue; // If interupted by signal should retry 
            perror("pipe_write_str: write");
            return -1;
        }
        written += n;
    }
    return 0;
}

/* ─────────────────────────────────────────────
 * pipe_read_line()
 *
 * Reads one byte at a time until '\n' or EOF.
 * ───────────────────────────────────────────── */

int pipe_read_line(int fd, char *buf, int maxlen) {
    int i = 0;
    char c;

    while (i < maxlen - 1) {
        ssize_t n = read(fd, &c, 1);
        if (n < 0) {
            if (errno == EINTR)  continue; // If signal interuupted, retry
            if (errno == EAGAIN) break;    // empty
            return -1;                     
        }
        if (n == 0) break; 

        buf[i++] = c;
        if (c == '\n') break; /* end of line */
    }

    buf[i] = '\0';
    return i;
}

/* ─────────────────────────────────────────────
 * parse_command()
 *
 * Splits a command string like "ls -la /tmp" into
 * argv = ["ls", "-la", "/tmp", NULL].
 *
 * We use strtok() on a copy of the string. The copy is stored
 * in argv[0]'s memory area so free_argv() can clean everything up
 * with two frees: free(copy) and free(argv).
 * ───────────────────────────────────────────── */
char **parse_command(const char *cmd_str, int *argc_out) {
    if (!cmd_str || strlen(cmd_str) == 0) return NULL;

    //All work done on a copy of the original
    char *copy = strdup(cmd_str);
    if (!copy) return NULL;

    //First counts tokens
    char *tmp = strdup(copy);
    int count = 0;
    char *tok = strtok(tmp, " \t\n");
    while (tok) { count++; tok = strtok(NULL, " \t\n"); }
    free(tmp);

    if (count == 0) { free(copy); return NULL; }

    //Allocate for argv
    char **argv = malloc((count + 1) * sizeof(char *));
    if (!argv) { free(copy); return NULL; }

    int idx = 0;
    tok = strtok(copy, " \t\n");
    while (tok) {
        argv[idx++] = tok;
        tok = strtok(NULL, " \t\n");
    }
    argv[idx] = NULL; 

    
    if (argc_out) *argc_out = count;

    
    argv[idx + 0] = copy; 

    char **argv2 = realloc(argv, (count + 2) * sizeof(char *));
    if (!argv2) { free(argv); free(copy); return NULL; }
    argv2[count]     = NULL;   //NULL Terminator
    argv2[count + 1] = copy;   //statishing copy pointer

    if (argc_out) *argc_out = count;
    return argv2;
}

/* ─────────────────────────────────────────────
 * free_argv()
 * ───────────────────────────────────────────── */
void free_argv(char **argv, int argc) {
    if (!argv) return;
    //the original copy is stored in argv+1
    if (argv[argc + 1]) free(argv[argc + 1]);
    free(argv);
}

/* ─────────────────────────────────────────────
 * create_output_dir()
 *
 * Builds the name:  <base_path>/outputs_<jobid>_<pid>_<YYYYMMDD>_<HHMMSS>
 * Creates the directory with mode 0755.
 * ───────────────────────────────────────────── */

int create_output_dir(int job_id, pid_t pid, const char *base_path,
                      char *out_dir_path) {
    time_t t = time(NULL);
    struct tm *tm_info = localtime(&t);

    snprintf(out_dir_path, MAX_PATH_LEN,
             "%s/outputs_%d_%d_%04d%02d%02d_%02d%02d%02d",
             base_path,
             job_id,
             (int)pid,
             tm_info->tm_year + 1900,
             tm_info->tm_mon  + 1,
             tm_info->tm_mday,
             tm_info->tm_hour,
             tm_info->tm_min,
             tm_info->tm_sec);

    if (mkdir(out_dir_path, 0755) < 0) {
        perror("create_output_dir: mkdir");
        return -1;
    }
    return 0;
}

/* ─────────────────────────────────────────────
 * redirect_output()
 *
 * Called in the child process (after fork, before exec).
 * Opens stdout_<jobid> and stderr_<jobid> inside dir_path,
 *
 * After dup2(), we close the original file descriptors because
 * the child process no longer needs them — STDOUT/STDERR now
 * point to the right files.
 * ───────────────────────────────────────────── */

int redirect_output(int job_id, const char *dir_path) {
    char path_buf[MAX_PATH_LEN];

    //Redirects stdout
    snprintf(path_buf, sizeof(path_buf), "%s/stdout_%d", dir_path, job_id);
    int fd_out = open(path_buf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_out < 0) { perror("redirect_output: open stdout"); return -1; }
    if (dup2(fd_out, STDOUT_FILENO) < 0) {
        perror("redirect_output: dup2 stdout"); close(fd_out); return -1;
    }
    close(fd_out);

    //Redirects stderr
    snprintf(path_buf, sizeof(path_buf), "%s/stderr_%d", dir_path, job_id);
    int fd_err = open(path_buf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_err < 0) { perror("redirect_output: open stderr"); return -1; }
    if (dup2(fd_err, STDERR_FILENO) < 0) {
        perror("redirect_output: dup2 stderr"); close(fd_err); return -1;
    }
    close(fd_err);

    return 0;
}