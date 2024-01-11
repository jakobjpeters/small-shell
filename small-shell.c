
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>

#define N_ARGS 512 + 1
#define LEN_INPUT 2048 + 1
#define DEV_NULL "/dev/null"

/*
A global variable to control `foreground-only mode`,
which needs to be accessable in `handle_sigtstp`.
*/
int background = 1;

void handle_sigtstp(int signo) {
    // toggle `foreground-only mode`
    background = !background;
    if (background) { write(STDOUT_FILENO, "\nExiting foreground-only mode\n", 30); }
    else { write(STDOUT_FILENO, "\nEntering foreground-only mode (& is now ignored)\n", 50); }
    return;
}

/*
Stores a parsed command.
*/
struct command {
    char* args[N_ARGS];
    char* input_file;
    char* output_file;
    int background;
};

/*
Creates and returns an empty `command`.
*/
struct command* create_command() {
    struct command* command = malloc(sizeof(struct command));
    for (int i = 0; i < N_ARGS; i++) {
        command->args[i] = NULL;
    }
    command->input_file = NULL;
    command->output_file = NULL;
    command->background = 0;
    return command;
}

/*
Free's a `command`'s memory.
*/
void free_command(struct command* command) {
    for (int i = 0; i < N_ARGS; i++) {
        free(command->args[i]);
    }
    free(command->input_file);
    free(command->output_file);
    free(command);
    return;
}

/*
A linked list to store PIDs.
*/
struct background_pids {
    int pid;
    struct background_pids* next;
};

/*
Insert a new PID into the given `background_pids`.
*/
struct background_pids* insert_pid(struct background_pids* background_pids, int pid) {
    struct background_pids* head = malloc(sizeof(struct background_pids));
    head->pid = pid;
    head->next = background_pids;
    return head;
}

/*
Recursively traverse the linked list `background_pids`,
killing each process associated with the PID and freeing each node.
*/
void free_background_pids(struct background_pids* background_pids) {
    if (background_pids) {
        if (background_pids->next) { free_background_pids(background_pids->next); }
        kill(background_pids->pid, SIGKILL);
        free(background_pids);
    }
    return;
}

/*
Return a `command` parsed from the given character array.
*/
struct command* parse(char* input) {
    // return an empty `command` if the input is a comment
    if (!strncmp(input, "#", 1)) { return create_command(); }

    // pid expansion
    char buffer[21];
    char* expanded_input = calloc(LEN_INPUT, sizeof(char));
    char* cache = expanded_input;

    // check each character for `$$`
    for (int i = 0; i < strlen(input); i++) {
        /*
        If current and next character is `$`,
        print the PID into `expanded_input` and skip the next character.
        */
        if (input[i] == '$' && input[i + 1] == '$') {
            sprintf(buffer, "%ld", getpid());
            strcat(expanded_input, buffer);
            i++;
        }
        // print the current character into `expanded_input`
        else {
            sprintf(buffer, "%c", input[i]);
            strcat(expanded_input, buffer);
        }
    }

    struct command* command = create_command();
    char* token;
    int arg_n = 0;
    int mode = 0;
    int i = strlen(expanded_input) - 2;

    /*
    If the second to last character is `&`,
    the command should run in the background.
    This character is also set to ` ` so that it is ignored during tokenizing.
    */
    if (i >= 0 && expanded_input[i] == '&') {
        command->background = background;
        expanded_input[i] = ' ';
    }

    /*
    There are 3 parsing modes.
    If the token if `<` or `>`, it will set `mode = 1` or `mode = 2`, respectively.

    `mode = 0` is the default and will insert `token` into `command->args`.
    `mode = 1` and `mode = 2` replace the value of
    `command->input_file` and `command->output_file`, respectively, with `token`.
    */
    while (token = strtok_r(NULL, " \n", &expanded_input)) {
        // change modes
        if (!strcmp(token, "<")) { mode = 1; }
        else if (!strcmp(token, ">")) { mode = 2; }
        // token is an argument
        else if (mode == 0) {
            command->args[arg_n] = calloc(strlen(token) + 1, sizeof(char));
            strcpy(command->args[arg_n], token);
            // increment a counter so that the next arg goes into the next index
            arg_n++;
        }
        // token is an input file
        else if (mode == 1) {
            command->input_file = calloc(strlen(token) + 1, sizeof(char));
            strcpy(command->input_file, token);
        }
        // token is an output file
        else if (mode == 2) {
            command->output_file = calloc(strlen(token) + 1, sizeof(char));
            strcpy(command->output_file, token);
        }
        else {
            printf("Error parsing input\n");
            fflush(stdout);
        }
    }

    // `cache` is a pointer to the beginning of `expanded_input`
    free(cache);
    return command;
}

/*
Redirects the given `file_name`.
`flag` determines what flags to open the file with,
which will either be `O_RDONLY` or `O_WRONLY | O_CREAT`.
If a new file is created, it is given full permissions.
If `flag == (O_WRONLY | O_CREAT)`, then the file is redirected to `stdout`.
Otherwise, the file is redirected to `stdin`.
Returns `0` on success and `1` on failure.
*/
int redirect(char* file_name, int flag, char* error, int* status) {
    int file_descriptor = open(file_name, flag, 0777);
    // `open` failed
    if (file_descriptor == -1) {
        printf("bash: %s: No such file or directory\n", file_name);
        fflush(stdout);
        *status = 1;
        return 1;
    }
    // `open` succeeded
    else {
        // redirect file to either `stdin` or `stdout`
        if (dup2(file_descriptor, flag == (O_WRONLY | O_CREAT)) == -1) {
            printf("Error redirecting %s\n", error);
            fflush(stdout);
            return 1;
        }
    }
    return 0;
}

/*
Uses macros to determine whether the status indicates a normal exit or exit by signal,
then prints the status with a message.
*/
void print_status(int status) {
    if (WIFEXITED(status)) { printf("exit value %d\n", WEXITSTATUS(status)); }
    else { printf("terminated by signal %d\n", WTERMSIG(status)); } // if (WIFSIGNALED(status))
    fflush(stdout);
}

/*
Registers the given signal and signal handler.
*/
void register_signal_handler(int signal, void (*handler)(int)) {
    struct sigaction signal_handler = {0};
    signal_handler.sa_handler = handler;
    sigaction(signal, &signal_handler, NULL);
    return;
}

/*
Executes the given `command`.
May modify `status` to include the most recent exit value or terminating signal.
May insert a new PID into `background_pids`.
Returns `0` to exit shell and `1` to continue.
*/
int run(struct command* command, int* status, struct background_pids** background_pids) {
    // empty command, do nothing
    if (!command->args[0]) {}
    // stop the `main` loop
    else if (!strcmp(command->args[0], "exit")) { return 0; }
    // change directories
    else if (!strcmp(command->args[0], "cd")) {
        if (command->args[1]) { chdir(command->args[1]); }
        // if no argument is given, default to the `HOME` directory
        else { chdir(getenv("HOME")); }
    }
    // print the most recent status
    else if (!strcmp(command->args[0], "status")) { print_status(*status); }
    // use `fork` and `exec` to run an unknown command in a child process
    else {
        pid_t child_pid = fork();
        switch (child_pid) {
            case -1: // error
                printf("Error forking process\n");
                fflush(stdout);
                break;
            case 0: // child
                // ignore sigtstp
                register_signal_handler(SIGTSTP, SIG_IGN);

                // default input/output file for background processes is `DEV_NULL`
                if (command->background) {
                    if (!command->input_file) { command->input_file = DEV_NULL; }
                    if (!command->output_file) { command->output_file = DEV_NULL; }
                }
                // foreground `SIGINT` should be handled normally
                else { register_signal_handler(SIGINT, SIG_DFL); }

                // redirect `stdin` as read-only
                if (command->input_file &&
                    redirect(command->input_file, O_RDONLY, "input", status)
                ) { exit(1); };
                // redirect `stdout` as write-only and create if non-existant
                if (command->output_file &&
                    redirect(command->output_file, O_WRONLY | O_CREAT, "output", status)
                ) { exit(1); };

                execvp(command->args[0], command->args);
                // `execvp` failed
                printf("bash: %s: Command not found\n", command->args[0]);
                fflush(stdout);
                exit(1);
                break;
            default: // parent
                // background
                if (command->background) {
                    // cache the new PID to `waitpid` later
                    *background_pids = insert_pid(*background_pids, child_pid);
                    printf("background pid is %d\n", child_pid);
                    fflush(stdout);
                }
                // foreground
                else {
                    // block `SIGTSTP` until child has finished
                    sigset_t sigtstp;
                    sigaddset(&sigtstp, SIGTSTP);
                    sigprocmask(SIG_BLOCK, &sigtstp, NULL);

                    // wait until child has finished
                    waitpid(child_pid, status, 0);
                    if (WIFSIGNALED(*status)) { print_status(*status); }

                    // unblock `SIGTSTP`
                    sigprocmask(SIG_UNBLOCK, &sigtstp, NULL);
                }
        }
    }
    return 1;
}

/*
Check to see if each background PID has exited or terminated.
If so, print a message and its status and remove it from the list of PIDs.
*/
struct background_pids* check_background(struct background_pids* background_pids) {
    struct background_pids* head = background_pids;
    struct background_pids* previous = NULL;
    int status;

    // continue until the end of the linked list
    while (background_pids) {
        // check the current PID
        pid_t result = waitpid(background_pids->pid, &status, WNOHANG);
        if (result) {
            // print message
            printf("background pid %d is done: ", background_pids->pid);
            print_status(status);

            // remove current PID from linked list
            if (previous) {
                previous->next = background_pids->next;
                free(background_pids);
                background_pids = previous;
            }
            else {
                head = background_pids->next;
                free(background_pids);
                background_pids = head;
            }
        }
        // iterate to the next node in the linked list
        previous = background_pids;
        if (background_pids) { background_pids = background_pids->next; }
    }
    return head;
}

/*
Run a small shell in a loop until the `exit` command is given.
First, check if any background processes have completed.
If so, print their PIDs and exit values and remove them from the list of background processes.
Next, prompt the user and read their input.
Finally, Parse the input to a command and run the command.
Upon exit, kill all remaining background processes.
*/
int main(int argc, char* argv[]) {
    // ignore `SIGINT`
    register_signal_handler(SIGINT, SIG_IGN);
    // register custom handler for `SIGTSTP`
    register_signal_handler(SIGTSTP, handle_sigtstp);

    struct command* command;
    struct background_pids* background_pids = NULL;
    char input[LEN_INPUT];
    int status = 0;
    int running = 1;

    while (running) {
        // print message when background processes have completed
        // update `background_pids` to remove completed PIDs
        background_pids = check_background(background_pids);

        // shell prompt
        printf(": ");
        fflush(stdout);

        // read command
        memset(input, '\0', sizeof(input));
        !fgets(input, sizeof(input), stdin);

        command = parse(input);
        /*
        Set `running` to `0` when the command `exit` is given,
        which stops the main loop.
        The previous `status` must be available for the next call,
        so it is updated and cached here.
        `background_pids` may be updated to include a new PID.
        */
        running = run(command, &status, &background_pids);
        free_command(command);
    }

    // also kills background processes
    free_background_pids(background_pids);
    return 0;
}

