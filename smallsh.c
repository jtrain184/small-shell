//
// A "small" shell
// With built in commands (cd, status, exit), I/O redirection
// and ability to run background processes with &.
// by Philip Jarrett
// CS 344, Winter 2018
//

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>  // For signals

// Functions
void shell_loop(void);
char *shell_read_line(void);
char **shell_split_line(char *line);
int shell_execute(char **args);
int shell_launch(char **args);
void background_check(int size, pid_t bgProcesses[]);
void kill_processes(int size, pid_t bgProcesses[]);
void catchSIGTSTP(int signo);

// Built-in commands functions
int shell_cd(char **args);
int shell_exit(char **args);
int shell_status(char **args);

/***                       ***/
/***    Global variables   ***/
/***                       ***/

// Flags used for redirection and background
int in;
int out;
int isBackground = 0;

// File names for I/O
char* inputFile;
char* outputFile;

// Sigaction structs
struct sigaction SIGINT_action = {0};
struct sigaction SIGTSTP_action = {0};

// Tracker for background processes
pid_t bgTracker[250] = {0};
int numBGProcesses = 0;

// Status variable, for passing to built in status
int status = -5;

// Background switich variable
int backgroundAllowed = 1;

int main(int argc, char **argv)
{
    // Signal handler setup
    SIGINT_action.sa_handler = SIG_IGN;
    sigfillset(&SIGINT_action.sa_mask);
    SIGINT_action.sa_flags = 0;

    SIGTSTP_action.sa_handler = catchSIGTSTP;
    SIGTSTP_action.sa_flags = SA_RESTART;

    // Register handlers
    sigaction(SIGINT, &SIGINT_action, NULL);
    sigaction(SIGTSTP, &SIGTSTP_action, NULL);

    // Run user control loop.
    shell_loop();

    return EXIT_SUCCESS;
} // end main


// Built in command list, to iterate over later
char *builtin_str[] = {
        "cd",
        "exit",
        "status"
};

// An array of function pointers to the built in command functions,
// will be used in the execution function later.
int (*builtin_func[]) (char **) = {
        &shell_cd,
        &shell_exit,
        &shell_status
};

// Helper function to return the number of built in commands in the array above
int shell_num_builtins() {
    return sizeof(builtin_str) / sizeof(char *);
}

/*** Implementing built-in functions cd, exit, and status ***/

// Built in cd command, changes to directory name given in agruments, if valid
// changes to home directory (defined in environment variable) if
// given ~ or no argument
int shell_cd(char **args)
{
    // Check arguments, if none, change to home directory
    if (args[1] == NULL) {
        if (chdir(getenv("HOME")) != 0) {
            perror("smallsh");
        }
    }
    // If ~ given, change to home direcotry
    else if (strcmp(args[1],"~") == 0) {
        if (chdir(getenv("HOME")) != 0) {
            perror("smallsh");
        }
    }
    // Else, invalid argument, error thrown
    else {
        if (chdir(args[1]) != 0) {
            perror("smallsh");
        }
    }
    return 1;
}

// Built in exit function, kills background processes and returns control to
// main function for exit.
int shell_exit(char **args)
{
    // Kill off any background processes before exiting
    kill_processes(numBGProcesses, bgTracker);
    // Return 0 to break loop and return control to end of main function
    return 0;
}

// Built in status command, returns exit value of process,
// or returns signal number if terminated by a signal.
int shell_status(char ** args)
{
    if(WIFEXITED(status)) {
        // The child process ended normally
        printf("exit value %d\n", WEXITSTATUS(status));
        fflush(stdout);
    }
    else if (WIFSIGNALED(status)) {
        // A signal terminated child process
        printf("terminated by signal %d\n", WTERMSIG(status));
        fflush(stdout);
    }
    return 1;
}

// Loop for user input into shell
void shell_loop(void)
{
    char *line;
    char **args;
    // Flag that holds return value of executed commands,
    // keeps loop running until 0 is returned from shell_execute function
    int shell_active;  

    do {
        // Reset flags for file redirection
        in = 0;
        out = 0;
        // Check background processes
        background_check(numBGProcesses, bgTracker);

        printf(": ");
        fflush(stdout);
        line = shell_read_line(); // Handles and stores user input
        args = shell_split_line(line); // Parses commands
        shell_active = shell_execute(args);  // Executes commands

        free(line);
        free(args);
    } while (shell_active);
}

// shell_read_line reads user input with getline()
// Expands any occurence of $$ to the shell PID
char *shell_read_line(void)
{
    char *line = NULL;
    size_t bufsize = 0; 
    getline(&line, &bufsize, stdin);

    static char buffer[4096];
    char *p = line;

    char pid[10];
    snprintf(pid, 10, "%d", (int)getpid());

    // Expanding $$ to parent PID
    while((p=strstr(p, "$$"))){
        strncpy(buffer, line, p-line);
        buffer[p-line] = '\0';
        strcat(buffer, pid);
        strcat(buffer, p+strlen("$$"));
        strcpy(line, buffer);
        p++;
    }

    return line;
}

// shell_split_line() parses line from user using strtok
// also handles logic for redirection and background commands
#define TOK_BUFFSIZE 64
#define TOK_DELIM " \t\r\n\a"
char **shell_split_line(char *line)
{
    int bufsize = TOK_BUFFSIZE, position = 0;
    char **tokens = malloc(bufsize * sizeof(char*));
    char *token;
    // Reset isBackground flag
    isBackground = 0;

    if (!tokens) {
        fprintf(stderr, "smallsh: allocation error for token buffer\n");
        exit(EXIT_FAILURE);
    }

    // Get first argument
    token = strtok(line, TOK_DELIM);

    // Get all successive arguments
    while (token != NULL) {

        // Resizes buffer if needed
        if (position >= bufsize) {
            bufsize += TOK_BUFFSIZE;
            tokens = realloc(tokens, bufsize * sizeof(char*));
            if (!tokens) {
                fprintf(stderr, "smshell: allocation error\n");
                exit(EXIT_FAILURE);
            }
        }

        // Logic to check for redirections and background commands

        // Output redirection
        if (strcmp(token, ">")==0){
            // Set output flag 
            out = 1;
            // Grab the file name (should be next argument)
            token = strtok(NULL,TOK_DELIM);
            outputFile = token;
        }

        // Input redirection
        else if (strcmp(token, "<")==0){
            // Set input flag
            in = 1;
            // Grab input source, should be next argument
            token = strtok(NULL,TOK_DELIM);
            inputFile = token;
        }

        // If not redirect or background, store argument in tokens array
        // to be returned for execution
        else {
            tokens[position] = token;
            position++;
        }

        // Look at next argument
        token = strtok(NULL, TOK_DELIM);
    }

    // Check for background command (&) is found at end of tokens array
    // Position > 0 checks to make sure the tokens array isn't empty
    if(position > 0 && strcmp(tokens[position-1], "&") ==0) {
        // Remove & (by setting location in array to null)
        // and set isBackground flag to 1
        tokens[--position] = NULL;
        // Only set isBackground if background mode is currently allowed
        if(backgroundAllowed) {
            isBackground = 1;
        }
    }

    else {
        // Set last argument in array to NULL, required for execvp()
        tokens[position] = NULL;
    }
    
    return tokens;
}

/* Function to handle execution of built in commands, comments, and blank lines */
int shell_execute(char **args)
{
    // A comment or emtpy line was entered.
    if (args[0] == NULL || strchr(args[0], '#') != NULL) {
        return 1;
    }

    int i;
    int num_builtins = shell_num_builtins();
    // Searches for built in commands
    for (i = 0; i < num_builtins; i++) {
        if (strcmp(args[0], builtin_str[i]) == 0) {
            // If built in, pass arguments to function pointer for that command
            return (*builtin_func[i])(args);
        }
    }

    // If command is not built in, passes arguments to be forked and executed
    return shell_launch(args);
}

// Function to handle fork/exec of non built in commands
int shell_launch(char **args)
{
    pid_t pid, wpid;
    pid = wpid = -5;  // Process id of forked child and wpid to store waitpid() value
    int inputFD = -5; // File descriptor for input file
    int outputFD = -5; // File descriptor for output file

    pid = fork();

    if (pid == 0) { // Now inside the child process
        // Change SIGTSTP to ignore for child processes
        SIGTSTP_action.sa_handler = SIG_IGN;
        sigaction(SIGTSTP, &SIGTSTP_action, NULL);
        // Change SIGINT back to default action for foreground processes
        // Background processes will continue to ignore SIGINT
        if(!isBackground) {
            SIGINT_action.sa_handler = SIG_DFL;
            sigaction(SIGINT, &SIGINT_action, NULL);
        }

        // Logic for I/O redirection, from lecture notes

        // Handle input redirection
        if (in == 1){
            inputFD = open(inputFile, O_RDONLY);
            // Error opening file
            if (inputFD == -1) { perror("input file open()"); exit(1); }
            // Otherwise, redirect stdin (0) to inputFile
            dup2(inputFD, 0);
        }

        // Handle output redirection
        if (out ==1){
            outputFD = open(outputFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            // Error opening file
            if (outputFD == -1) { perror("output file open()"); exit(1); }
            // Otherwise, redirect stdout (1) to outputFile
            dup2(outputFD, 1);
        }

        // Handle background I/O when not specified
        // direct /dev/null to STDIN for input
        if(isBackground && !in){
            inputFD = open("/dev/null", O_RDONLY);
            if (inputFD == -1) { perror("input file open()"); exit(1); }
            // Redirect input to STDIN from /dev/null
            dup2(inputFD, 0);
        }
        // Direct output from stdout to /dev/null
        if (isBackground && ! out){
            outputFD = open("/dev/null", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            // Error opening file
            if (outputFD == -1) { perror("output file open()"); exit(1); }
            // Otherwise, redirect stdout to /dev/null
            dup2(outputFD, 1);
        }
        // Pass args to execvp() and check for error
        if (execvp(args[0], args) == -1) {
            perror("smallsh");
        }
        // Should only reach if execvp fails
        exit(EXIT_FAILURE);

    } else if (pid < 0) {  // Error forking
        perror("smallsh");

    } else {  // This is the parent process
        if(isBackground) {
            // Print background processs PID
            printf("background pid is %d\n", pid);
            fflush(stdout);
            // Add background process to tracking array
            bgTracker[numBGProcesses++] = pid;
        }
        // Otherwise, not a background process
        // Loop and wait for foreground process to finish
        else{
            do {
                wpid = waitpid(pid, &status, WUNTRACED);
            } while (!WIFEXITED(status) && !WIFSIGNALED(status));

        // Catch and print signal
            if (WIFSIGNALED(status)){
                printf("terminated by signal %d\n", status);
                fflush(stdout);
            }
        }
    }

    // Clean up files, if any opened
    // originally set to -5 so here chcking to see if they were
    // set to something different (i.e. files opened)
    if(inputFD != -5) close(inputFD);
    if(outputFD != -5) close(outputFD);

    // Returns 1 to keep user loop going
    return 1;
}

// Function to scan list of background processes and return status if any have ended
void background_check(int size, pid_t bgProcesses[]){
    int i, childExitMethod;

    for (i = 0; i < size; i++){
        // Check to see if process ended
        pid_t wpid = waitpid(bgProcesses[i], &childExitMethod, WNOHANG);

        if(wpid == bgProcesses[i]){
            if(WIFEXITED(childExitMethod)) {
                // The child process ended normally
                printf("background pid %d is done: exit value %d\n", wpid, WEXITSTATUS(childExitMethod));
                fflush(stdout);
            }
            else if (WIFSIGNALED(childExitMethod)){
                // A signal terminated child process
                printf("background pid %d is done: terminated by signal %d\n", wpid, WTERMSIG(childExitMethod));
                fflush(stdout);
            }
        }
    }
}

// Function used to cycle through and kill background processes
void kill_processes(int size, pid_t bgProcesses[]){
    int i;

    for (i = 0; i < size; i++) {
        kill(bgProcesses[i], SIGKILL);
    }
}

// Signal handler for SIGTSTP, turns on/off foreground-only mode
void catchSIGTSTP(int signo){
    // If background currently allowed, toggle off and send message
    if(backgroundAllowed == 1)
    {
        char* message = "\nEntering foreground-only mode (& is now ignored)\n: ";
        write(STDOUT_FILENO, message, 52);
        fflush(stdout);
        backgroundAllowed = 0;
    }
    // If background currently not allowed, toggle on and send message
    else if(backgroundAllowed == 0)
    {
        char* message = "\nExiting foreground-only mode\n: ";
        write(STDOUT_FILENO, message, 32);
        fflush(stdout);
        backgroundAllowed = 1;
    }
}