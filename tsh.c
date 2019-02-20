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
    sigset_t ourmask;
    sigset_t oldmask;
    
    Sigemptyset(&ourmask);
    Sigemptyset(&oldmask);
    Sigaddset(&ourmask, SIGCHLD);
    Sigaddset(&ourmask, SIGINT);
    Sigaddset(&ourmask, SIGTSTP);
    
    // Parse command line
    parse_result = parseline(cmdline, &token);

    if (parse_result == PARSELINE_ERROR || parse_result == PARSELINE_EMPTY)
    {
        return;
    }
	
	// 1). Check if parsed_result is BUILTIN or not.
	// 2). If parsed_result is not BUILTIN, run as an executable program.
	// 3). Check if it should be run in FG or BG mode.
    switch (token.builtin) {
        case BUILTIN_QUIT:
            exit(0);
            break;
        case BUILTIN_JOBS:
            listjobs(job_list, 1); // Print the job list on stdout.
            break;
        case BUILTIN_BG:
            break;
        case BUILTIN_FG:
            break;
        case BUILTIN_NONE:
            ;
            pid_t pid;
            Sigprocmask(SIG_BLOCK, &ourmask, &oldmask); // Block signals in mask before forking.
            pid = Fork();
            if (pid == 0) {
                Setpgid(0, 0); // puts the child in a new process group with identical group ID to its PID.
				
                // Resets signal handlers to default behavior.
                Sigprocmask(SIG_UNBLOCK, &ourmask, NULL);
                Signal(SIGINT, SIG_DFL);
                Signal(SIGCHLD, SIG_DFL);
                Signal(SIGTSTP, SIG_DFL);
                
                Execve(token.argv[0], token.argv, environ);
                exit(0);
            } else if (parse_result == PARSELINE_FG) {
                // Handle child process in foreground.
                addjob(job_list, pid, FG, cmdline);
                
                // Unblocks sigs in mask until a signal whose action is to 
                // invoke a signal handler or to terminate a process is received.
//                 Sigprocmask(SIG_UNBLOCK, &ourmask, NULL);
                Sigsuspend(&oldmask); 
                Sigprocmask(SIG_UNBLOCK, &ourmask, NULL);
//                 printf("Sigsuspend finished!\n");
//                 fflush(stdout);
                
            } else if (parse_result == PARSELINE_BG) {
                // Handle child process in background.
                addjob(job_list, pid, BG, cmdline);
                struct job_t *job = getjobpid(job_list, pid);
                Sigprocmask(SIG_UNBLOCK, &ourmask, NULL);
                
                printf("[%d] (%d) %s\n", job->jid, job->pid, cmdline);
            }
            break;
    }
	
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
//     printf("Sigchild called!\n");
//     fflush(stdout);
    pid_t pid;
    int status;
    sigset_t ourmask;
    
    Sigemptyset(&ourmask);
    Sigaddset(&ourmask, SIGCHLD);
    Sigaddset(&ourmask, SIGINT);
    Sigaddset(&ourmask, SIGTSTP);
    
    int saved_errno = errno;
    if ((pid = Waitpid((pid_t)(-1), &status, WNOHANG)) > 0 || 
        (pid = Waitpid((pid_t)(-1), &status, WUNTRACED)) > 0) {
//         printf("Statement entered!\n");
//         fflush(stdout);
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            Sigprocmask(SIG_BLOCK, &ourmask, NULL);
            deletejob(job_list, pid); // Delete from job_list after child reaped.
//             printf("Job deleted!\n");
//             fflush(stdout);
            Sigprocmask(SIG_UNBLOCK, &ourmask, NULL);
        }
    }
    errno = saved_errno;
    return;
}

/* 
 * <What does sigint_handler do?>
 */
void sigint_handler(int sig) 
{
    Kill(fgpid(job_list), sig); // Group id needs to be preceded by "-" without quotes.
    return;
}

/*
 * <What does sigtstp_handler do?>
 */
void sigtstp_handler(int sig) 
{
    return;
}
