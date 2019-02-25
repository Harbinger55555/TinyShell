/* 
 * tsh - A tiny shell program with job control
 * <The line above is not a sufficient documentation.
 *  You will need to write your program documentation.>
 */

#include "tsh_helper.h"

/*
 * If DEBUG is defined, enable contracts and printing on dbg_printf.
 */
#ifdef DEBUG
/* When debugging is enabled, these form aliases to useful functions */
#define dbg_printf(...) printf(__VA_ARGS__)
#define dbg_requires(...) assert(__VA_ARGS__)
#define dbg_assert(...) assert(__VA_ARGS__)
#define dbg_ensures(...) assert(__VA_ARGS__)
#else
/* When debugging is disabled, no code gets generated for these */
#define dbg_printf(...)
#define dbg_requires(...)
#define dbg_assert(...)
#define dbg_ensures(...)
#endif

/* Function prototypes */
void eval(const char *cmdline);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);
void sigquit_handler(int sig);
void init_mask(sigset_t *newmask);
pid_t get_sig_gpid();
void set_sig_defaults();
void print_kill_job(int jid, pid_t pid, int sig);
int Npow10(int N, int n);
int gjid_past_perc(char* argv1);
void builtin_bgfg(char* argv1, sigset_t newmask, job_state state);

volatile sig_atomic_t sig_chld = 0; // When a SIGCHLD signal arrives, set this variable.
int saved_stdout; // To save stdout before being dup with another file descriptor.
int saved_stdin; // To save stdin before being dup with another file descriptor.

/*
 * <Write main's function header documentation. What does main do?>
 * "Each function should be prefaced with a comment describing the purpose
 *  of the function (in a sentence or two), the function's arguments and
 *  return value, any error cases that are relevant to the caller,
 *  any pertinent side effects, and any assumptions that the function makes."
 */
int main(int argc, char **argv) 
{
    char c;
    char cmdline[MAXLINE_TSH];  // Cmdline for fgets
    bool emit_prompt = true;    // Emit prompt (default)

    // Redirect stderr to stdout (so that driver will get all output
    // on the pipe connected to stdout)
    Dup2(STDOUT_FILENO, STDERR_FILENO);

    // Parse the command line
    while ((c = getopt(argc, argv, "hvp")) != EOF)
    {
        switch (c)
        {
        case 'h':                   // Prints help message
            usage();
            break;
        case 'v':                   // Emits additional diagnostic info
            verbose = true;
            break;
        case 'p':                   // Disables prompt printing
            emit_prompt = false;  
            break;
        default:
            usage();
        }
    }

    // Install the signal handlers
    Signal(SIGINT,  sigint_handler);   // Handles ctrl-c
    Signal(SIGTSTP, sigtstp_handler);  // Handles ctrl-z
    Signal(SIGCHLD, sigchld_handler);  // Handles terminated or stopped child

    Signal(SIGTTIN, SIG_IGN);
    Signal(SIGTTOU, SIG_IGN);

    Signal(SIGQUIT, sigquit_handler); 

    // Initialize the job list
    initjobs(job_list);

    // Execute the shell's read/eval loop
    while (true)
    {
        if (emit_prompt)
        {
            printf("%s", prompt);
            fflush(stdout);
        }
        
        if ((fgets(cmdline, MAXLINE_TSH, stdin) == NULL) && ferror(stdin))
        {
            app_error("fgets error");
        }

        if (feof(stdin))
        { 
            // End of file (ctrl-d)
            printf ("\n");
            fflush(stdout);
            fflush(stderr);
            return 0;
        }
        
        // Remove the trailing newline
        cmdline[strlen(cmdline)-1] = '\0';
        
        // Evaluate the command line
        eval(cmdline);
        
        fflush(stdout);
    } 
    
    return -1; // control never reaches here
}


/* Handy guide for eval:
 *
 * If the user has requested a built-in command (quit, jobs, bg or fg),
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.
 * Note: each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.
 */

/* 
 * <What does eval do?>
 */
void eval(const char *cmdline) 
{
    parseline_return parse_result;     
    struct cmdline_tokens token;
    
    sigset_t newmask;
    sigset_t oldmask;
    init_mask(&newmask);
    Sigemptyset(&oldmask);
    
    // Parse command line
    parse_result = parseline(cmdline, &token);
    if (parse_result == PARSELINE_ERROR || parse_result == PARSELINE_EMPTY)
    {
        return;
    }
    
    /* Save current stdin and stdout for use later */
    saved_stdin = dup(STDIN_FILENO);
    saved_stdout = dup(STDOUT_FILENO);
    
    if (token.infile) {
        // Redirect filein to stdin.
        FILE *infile = fopen(token.infile, "r");
        dup2(fileno(infile), STDIN_FILENO);
    }

    if (token.outfile) {
        // Redirect stdout to fileout.
        FILE *outfile = fopen(token.outfile, "w");
        dup2(fileno(outfile), STDOUT_FILENO);
    }
	
	// 1). Check if parsed_result is BUILTIN or not.
	// 2). If parsed_result is not BUILTIN, run as an executable program.
	// 3). Check if it should be run in FG or BG mode.
    switch (token.builtin) {
        case BUILTIN_QUIT:
            exit(0);
            break;
        case BUILTIN_JOBS:
            Sigprocmask(SIG_BLOCK, &newmask, NULL); // Block signals before accessing job list.
            listjobs(job_list, STDOUT_FILENO); // Print the job list on stdout.
            Sigprocmask(SIG_UNBLOCK, &newmask, NULL); // Unblock signals after accessing job list.
            break;
        case BUILTIN_BG:
            ;
            // bg only takes in one job at a time (bg %n where n is the job id).
            // The bg job command restarts job by sending it a SIGCONT signal, 
            // and then runs it in the background.
            builtin_bgfg(token.argv[1], newmask, BG);
            break;
        case BUILTIN_FG:
            ;
            // fg only takes in one job at a time (fg %n where n is the job id).
            // The fg job command restarts job by sending it a SIGCONT signal, 
            // and then runs it in the foreground.
            builtin_bgfg(token.argv[1], newmask, FG);
            while(!sig_chld)
                Sigsuspend(&oldmask);
            break;
        case BUILTIN_NONE:
            ;
            pid_t pid;
            Sigprocmask(SIG_BLOCK, &newmask, &oldmask); // Block signals in mask before forking.
            pid = Fork();
            if (pid == 0) {
                Setpgid(0, 0); // puts the child in a new process group with identical group ID to its PID.
				
                // Resets signal handlers to default behavior.
                set_sig_defaults();
                Sigprocmask(SIG_UNBLOCK, &newmask, NULL);
                
                Execve(token.argv[0], token.argv, environ);
                exit(0);
            } else if (parse_result == PARSELINE_FG) {
                sig_chld = 0; // Resets the sig_chld volatile.
                // Handle child process in foreground.
                addjob(job_list, pid, FG, cmdline);
                
                // Suspends the shell until a signal whose action is to 
                // invoke a signal handler or to terminate a process is received.
                while(!sig_chld) {
                    Sigsuspend(&oldmask);
                }
                
                Sigprocmask(SIG_UNBLOCK, &newmask, NULL);
            } else if (parse_result == PARSELINE_BG) {
                // Handle child process in background.
                addjob(job_list, pid, BG, cmdline);
                struct job_t *job = getjobpid(job_list, pid);
                
                printf("[%d] (%d) %s\n", job->jid, job->pid, cmdline);
                Sigprocmask(SIG_UNBLOCK, &newmask, NULL);
            }
            break;
    }
    
    /* Restore stdout and stdin */
    dup2(saved_stdout, STDOUT_FILENO);
    close(saved_stdout);
    dup2(saved_stdin, STDIN_FILENO);
    close(saved_stdin);
	
    return;
}

/*****************
 * Signal handlers
 *****************/

/* 
 * <What does sigchld_handler do?>
 */
void sigchld_handler(int sig) 
{
    pid_t pid;
    int status;
    job_state state;
    sigset_t newmask;
    init_mask(&newmask);
    
    // process doesnt exist if pid < 0.
    // if pid == 0, no change in its state yet.
    while ((pid = waitpid((pid_t)(-1), &status, WNOHANG | WUNTRACED)) > 0) {
        Sigprocmask(SIG_BLOCK, &newmask, NULL);
        struct job_t *job = getjobpid(job_list, pid);
        int jid = job->jid;
        state = job->state;
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            deletejob(job_list, pid); // Delete from job_list after child reaped.
            if (WIFSIGNALED(status)) {
                print_kill_job(jid, pid, WTERMSIG(status));
            }
        } else if (WIFSTOPPED(status)) {
            // Change the status of pid in job list.
            job->state = ST;
            print_kill_job(jid, pid, WSTOPSIG(status));
        }
        if (state == FG) {
            sig_chld = 1; // Successful SIGCHLD handling of fg process allows parent to exit suspend.
        }
        Sigprocmask(SIG_UNBLOCK, &newmask, NULL);
    }
    return;
}

/* 
 * <What does sigint_handler do?>
 */
void sigint_handler(int sig) 
{
    Kill(get_sig_gpid(), SIGINT); // Send Kill SIGINT to pid.
    return;
}

/*
 * <What does sigtstp_handler do?>
 */
void sigtstp_handler(int sig) 
{
    Kill(get_sig_gpid(), SIGTSTP); // Send Kill SIGTSTP to pid.
    return;
}

void init_mask(sigset_t *newmask)
{
    Sigemptyset(newmask);
    Sigaddset(newmask, SIGCHLD);
    Sigaddset(newmask, SIGINT);
    Sigaddset(newmask, SIGTSTP);
    return;
}

pid_t get_sig_gpid() 
{
    pid_t pid;
    sigset_t newmask;
    init_mask(&newmask);
    
    Sigprocmask(SIG_BLOCK, &newmask, NULL);
    pid = -fgpid(job_list); // Group id needs to be preceded by "-" without quotes.
    Sigprocmask(SIG_UNBLOCK, &newmask, NULL);
    return pid;
}

void set_sig_defaults()
{
    Signal(SIGINT, SIG_DFL);
    Signal(SIGCHLD, SIG_DFL);
    Signal(SIGTSTP, SIG_DFL);
    return;
}

void print_kill_job(int jid, pid_t pid, int sig)
{
    Sio_puts("Job [");
    Sio_putl(jid);
    Sio_puts("] (");
    Sio_putl(pid);
    Sio_puts(") ");
    switch (sig) {
        case SIGINT:
            Sio_puts("terminated");
            break;
        case SIGTSTP:
            Sio_puts("stopped");
            break;
        default:
            break;
    }
    Sio_puts(" by signal ");
    Sio_putl(sig);
    Sio_puts("\n");
    return;
}

// Nx10^n
int Npow10(int N, int n)
{
    N <<= n;
    while(n--) N += N << 2;
    return N/10;
}

int gjid_past_perc(char* argv1) 
{
    char* job_str = argv1 + 1; // To get rid of the %
    int jid_digits = strlen(argv1) - 1;
    int count, jid = 0;
    for(count = 0; count < jid_digits; count++) {
        jid = jid * Npow10(10, count) + atoi(job_str++);
    }
    return jid;
}

void builtin_bgfg(char* argv1, sigset_t newmask, job_state state) 
{
    Sigprocmask(SIG_BLOCK, &newmask, NULL); // Block signals before accessing job list.
    int jid = gjid_past_perc(argv1);
            
    struct job_t *job = getjobjid(job_list, jid);
    if (job && job->state == ST) {
        Kill(-job->pid, SIGCONT); // Will halt program if SIGCONT fails.
        if (state == FG) {
            sig_chld = 0; // Resets the sig_chld volatile.
            job->state = FG;
        } else if (state == BG) {
            job->state = BG;
            printf("[%d] (%d) %s\n", jid, job->pid, job->cmdline);
        }
    }
    Sigprocmask(SIG_UNBLOCK, &newmask, NULL); // Unblock signals after accessing job list.
    return;
}
