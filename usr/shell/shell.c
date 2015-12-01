#include "shell.h"

#define BACKSPACE   8
#define ESCAPE      27
#define DELETE      127
#define SIGINT      3  
#define SIGEOF      26
#define RETURN      13

static void shell_input(char *buf, size_t len) 
{
    int count = 0;
    int c;

    printf("$ ");
    while (count < len) {
        c = getchar();
        if (count > 0) {
            if (c == DELETE) {
                c = BACKSPACE;
            }

            if (c == BACKSPACE) {
                count--;
                printf("%c %c", BACKSPACE, BACKSPACE);
            }
        }

        if (c == RETURN && count > 0) {
            printf("\r\n");
            break;
        }

        if (c > 31 && c < 127) {
            putchar(c);
            memcpy(&buf[count++], &c, 1);
        }
        //debug_printf("%d", c);
    }
}

static void get_argv(char *str, char **argv, int *args)
{
    char *tok; 
    int i;
    const char *delim = " ";

    tok = strtok(str, delim);
    
    if (tok != NULL) {
        argv[0] = tok; 
    }

    for (i = 1; (tok = strtok(NULL, delim)) != NULL; i++) {
        argv[i] = tok;
    }

    *args = i;
}

static void echo(char *rbuf)
{
    printf("%s\r\n", rbuf);
}

static void run_memtest(char *input_argv) 
{
    char *argv; 
    int args;

    get_argv(input_argv, &argv, &args);

    if (args > 1) {
        printf("`run_memtest` should only be run with ONE argument!\r\n");
        return;
    }

    perform_array_test(SMALL_CHUMP_SIZE, atoi(&argv[0]));
    printf("run_memtest completed...\r\n");
}

static void oncore(char *input_argv) 
{   
    
    printf("oncore excecution complete...\r\n");
}

int main(int argc, char *argv[])
{
    char input_buf[256];
    char *input_argv;
    char cmd[64];

    memset(input_buf, '\0', 256);

    while (true) {
        shell_input(input_buf, 256);
        //printf("DEBUG: %s\r\n", input_buf);
        input_argv = headstr(input_buf, cmd, ' ');
        //printf("DEBUG CMD: %s\r\n", cmd);

        if (is_user_app(cmd)) {
            // execute spawn user application and provide arguments.
            printf("executing %s...\r\n", cmd);
            continue;
        } 

        if (strcmp(cmd, "echo") == 0) {
            echo (input_argv);
        } else if (strcmp(cmd, "exit") == 0) {
            printf("exiting shell... goodbye\r\n");
            break;
        } else if (strcmp(cmd, "run_memtest") == 0) {
            // forks a thread and runs a memory test.
            run_memtest(input_argv);
        } else if (strcmp(cmd, "oncore") == 0) {
            // eg. oncore 1 [USR_APP]
            // eg. oncore 2 [USR_APP]
            // err = aos_rpc_process_spawn();
            // if (err_is_fail(err)) {

            // }
        } else if (strcmp(cmd, "ps") == 0) {
            printf("`ps` command runned...\r\n");
        } else if (strcmp(cmd, "list") == 0) {
            list_app();
        } else {
            printf("unknown command. try again\r\n");
        }

        memset(input_buf, '\0', 256);
        memset(cmd, '\0', 256);
    }
    return 0;
}
