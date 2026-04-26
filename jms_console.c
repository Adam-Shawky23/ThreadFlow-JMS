#include "jms_common.h"

/* ─────────────────────────────────────────────
 * read_response()
 *
 * Reads lines from fd until it sees END_SENTINEL, printing each line.
 * This is how the console knows when the coordinator has finished
 * sending its response (the coordinator always ends with "END\n").
 * ───────────────────────────────────────────── */

static void read_response(int fd) {
    char line[BUF_SIZE];

    while (1) {
        int n = pipe_read_line(fd, line, sizeof(line));
        if (n <= 0) {
            //EOF
            break;
        }

        //Stops when sees END
        if (strcmp(line, END_SENTINEL) == 0) break;

        printf("%s", line);
    }
    fflush(stdout);
}

/* ─────────────────────────────────────────────
 * send_command()
 *
 * Sends a command line to jms_coord and reads back the response.
 * Adds a newline to the command if it doesn't have one.
 * ───────────────────────────────────────────── */

static void send_command(int fd_write, int fd_read, const char *cmd) {
    pipe_write_str(fd_write, cmd);

    //Make sure it ends with a newline
    if (cmd[strlen(cmd) - 1] != '\n') {
        pipe_write_str(fd_write, "\n");
    }

    read_response(fd_read);
}

/* ─────────────────────────────────────────────
 * run_commands()
 *
 * Reads commands line by line from input,
 * sends each one to the coordinator, prints the response.
 * ───────────────────────────────────────────── */

static void run_commands(FILE *input, int fd_write, int fd_read,
                         int is_interactive) {
    char line[BUF_SIZE];

    while (1) {
        if (is_interactive) {
            printf("jms> ");
            fflush(stdout);
        }

        if (fgets(line, sizeof(line), input) == NULL) break; // EOF

        char trimmed[BUF_SIZE];
        strncpy(trimmed, line, sizeof(trimmed));
        trimmed[strcspn(trimmed, "\n")] = '\0';

        //skip blank lines
        if (strlen(trimmed) == 0) continue;

        if (strcmp(trimmed, "exit") == 0 || strcmp(trimmed, "quit") == 0) break;

        send_command(fd_write, fd_read, line);
    }
}

/* ─────────────────────────────────────────────
 * main()
 * ───────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    char  pipe_in_name[MAX_PATH_LEN]  = "";  // the -w argument to write
    char  pipe_out_name[MAX_PATH_LEN] = "";  // the -r argument to read
    char  ops_file_name[MAX_PATH_LEN] = "";  // the -o argument
    int   opt;

    // Parse arguments
    while ((opt = getopt(argc, argv, "w:r:o:")) != -1) {
        switch (opt) {
            case 'w':
                strncpy(pipe_in_name, optarg, MAX_PATH_LEN - 1);
                break;
            case 'r':
                strncpy(pipe_out_name, optarg, MAX_PATH_LEN - 1);
                break;
            case 'o':
                strncpy(ops_file_name, optarg, MAX_PATH_LEN - 1);
                break;
            default:
                exit(EXIT_FAILURE);
        }
    }

    // Validate required arguments 
    if (strlen(pipe_in_name) == 0 || strlen(pipe_out_name) == 0) {
        exit(EXIT_FAILURE);
    }

    /* ── Open the named pipes ──
     *
     * Open jms_out (read end) with O_NONBLOCK first so we don't block
     * waiting for coord to open its write end — coord already has it open
     * via O_RDWR. Then remove O_NONBLOCK so actual reads block normally.
     * Open jms_in (write end) normally — coord already has its read end open. */

    int fd_read = open(pipe_out_name, O_RDONLY | O_NONBLOCK);
    if (fd_read < 0) {
        perror("jms_console: open read pipe");
        exit(EXIT_FAILURE);
    }
    
    // Remove O_NONBLOCK so reads block while waiting for responses 
    int rdflags = fcntl(fd_read, F_GETFL);
    fcntl(fd_read, F_SETFL, rdflags & ~O_NONBLOCK);

    int fd_write = open(pipe_in_name, O_WRONLY);
    if (fd_write < 0) {
        perror("jms_console: open write pipe");
        exit(EXIT_FAILURE);
    }
    

    // Run commands from ops file (if provided) 
    if (strlen(ops_file_name) > 0) {
        FILE *f = fopen(ops_file_name, "r");
        if (f == NULL) {
            perror("jms_console: open operations file");
        } else {
            run_commands(f, fd_write, fd_read, 0 /* not interactive */);
            fclose(f);
        }
    }

    int is_terminal = isatty(STDIN_FILENO);
    run_commands(stdin, fd_write, fd_read, is_terminal);

    // Cleanup 
    close(fd_write);
    close(fd_read);

    return 0;
}