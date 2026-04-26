#ifndef JMS_COMMON_H
#define JMS_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <errno.h>

//Pipe names
#define PIPE_IN  "jms_in"   // console writes here, coord reads  
#define PIPE_OUT "jms_out"  // coord writes here,  console reads 

// Sentinel that marks the end of every response from coord to console.
#define END_SENTINEL "END\n"

//Buffer limits
#define MAX_JOBS       1024   // maximum total jobs tracked by coord  
#define MAX_POOLS      128    // maximum simultaneous pool processes   
#define BUF_SIZE       2048   // read/write buffer    
#define MAX_CMD_LEN    1024   // maximum length of a submitted command 
#define MAX_PATH_LEN   1024   // maximum path length                   

//Job Status 
typedef enum {
    JOB_ACTIVE    = 0,
    JOB_FINISHED  = 1,
    JOB_SUSPENDED = 2
} JobStatus;

//Job Record
typedef struct {
    int       job_id;           //Logical Id
    pid_t     pid;              //Linux Pid
    JobStatus status;           
    time_t    start_time;       //unix timestamp
    int       pool_idx;         //index into pools
    char      cmd[MAX_CMD_LEN]; 
} Job;

//Pool record
typedef struct {
    pid_t  pid;                     // Linux Pid
    int    fd_in;                   // coord writes job commands to pool here
    int    fd_out;                  // coord reads pool reports from here 
    int    jobs_assigned;           // how many jobs sent to this pool so far
    int    jobs_capacity;           // max jobs this pool will accept (=jobs_pool arg) 
    int    active;                  // 1 = pool is still running, 0 = exited   
    char   pipe_in_path[MAX_PATH_LEN];  // filesystem path of pool's input pipe  
    char   pipe_out_path[MAX_PATH_LEN]; // filesystem path of pool's output pipe 
} Pool;


// Pool ↔ Coord protocol messages

#define POOL_MSG_RUN      "run"
#define POOL_MSG_STARTED  "started"
#define POOL_MSG_FINISHED "finished"
#define POOL_MSG_DONE     "done"


int pipe_write_str(int fd, const char *msg);

int pipe_read_line(int fd, char *buf, int maxlen);

char **parse_command(const char *cmd_str, int *argc_out);

void free_argv(char **argv, int argc);

int create_output_dir(int job_id, pid_t pid, const char *base_path,
                      char *out_dir_path);

int redirect_output(int job_id, const char *dir_path);

#endif 