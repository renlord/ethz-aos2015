#include "shell.h"

#define BACKSPACE   8
#define ESCAPE      27
#define DELETE      127
#define SIGINT      3  
#define SIGEOF      26
#define RETURN      13
#define FORMFEED    12

static struct {
    uint8_t         height;
    coreid_t        coreid;
    const char      *name;
    const char      *version;
} shell_config = {
    .height = 33,
    .name = "pikachu",
    .version = "0.1a"
};

static const char *pikachu_img = " \
###############################################\n \
     .__                           __. \n \
  \\ `\\~~---..---~~~~~~--.---~~| /   \n \
   `~-.   `                   .~         _____ \n \
       ~.                .--~~    .---~~~    / \n \
        / .-.      .-.      |  <~~        __/ \n \
       |  |_|      |_|       \\  \\     .--' \n \
      /-.      -       .-.    |  \\_   \\_ \n \
      \\-'   -..-..-    `-'    |    \\__  \\_ \n \
       `.                     |     _/  _/ \n \
         ~-                .,-\\   _/  _/ \n \
        /                 -~~~~\\ /_  /_ \n \
       |               /   |    \\  \\_  \\_ \n \
       |   /          /   /      | _/  _/ \n \
       |  |          |   /    .,-|/  _/ \n \
       )__/           \\_/    -~~~| _/ \n \
         \\                      /  \\ \n \
          |           |        /_---` \n \
          \\    .______|      ./ \n \
          (   /        \\    /   \n \
          `--'          /__/ \n \
#############################################\n \
>>> GLORIOUS PIKACHU SHELL 0.1a <<< \n \
#############################################\n \
";

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
            printf("\n");
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
    printf("%s\n", rbuf);
}

static void run_memtest(char *input_argv) 
{
    char *argv; 
    int args;

    get_argv(input_argv, &argv, &args);

    if (args > 1) {
        printf("`run_memtest` should only be run with ONE argument!\n");
        return;
    }

    perform_array_test(SMALL_CHUMP_SIZE, atoi(&argv[0]));
    printf("run_memtest completed...\n");
}

static void ps(char *input_argv)
{
    domainid_t *pids;
    size_t pid_count;
    errval_t err = aos_rpc_process_get_all_pids(&local_rpc, &pids, &pid_count);
    if(err_is_fail(err)){
        err_print_calltrace(err);
        abort();
    }

    const char *divide = "+--------------------------+\n";
    printf("%s| Number of processes: %*d |\n%s", divide, 3,pid_count, divide);

    for(uint32_t j = 0; j < pid_count; j++){
        char *name_buf = (char *)malloc(20);
        err = aos_rpc_process_get_name(&local_rpc, pids[j], &name_buf);
        if (err_is_fail(err)) {
            DEBUG_ERR(err, "failed to get process name\n");
            err_print_calltrace(err);
            abort();
        }
        assert(name_buf != NULL);
        printf("| %*d. %-15s %*d |\n", 3, j, name_buf, 3, pids[j]);
        memset(name_buf, '\0', 20);
    }
    printf(divide);
}

static void clear(void)
{
    for (int i = 0; i < shell_config.height; i++) {
        putchar(FORMFEED);
    }
}

static errval_t spawn(char *argv_str, coreid_t coreid, domainid_t *new_pid)
{
    errval_t err = aos_rpc_process_spawn(&local_rpc, argv_str, coreid, new_pid);
    if(err_is_fail(err)){
        err_print_calltrace(err);
        return err;
    }
    return SYS_ERR_OK;
}

static errval_t oncore(char *input_argv, domainid_t *new_pid) 
{   
    char head[10];
    char *argv = headstr(input_argv, head, ' ');
    char *check;
    coreid_t coreid = strtol(head, &check, 10);
    if (check == head) {
        printf("oncore argument wrong! core selection is invalid\n");
        return SYS_ERR_OK;
    }

    if (coreid > 1 || coreid < 0) {
        printf("oncore argument invalid! There are only 2 usable cores on the "
            "pandaboard!\n");
        return SYS_ERR_OK;
    }

    return spawn(argv, coreid, new_pid);
}


static void set_shell(char *argv_str) 
{
    char cmd[64];
    char *input_argv = headstr(argv_str, cmd, ' ');

    if (strcmp(cmd, "height") == 0) {
        char *argv[1];
        int args;
        get_argv(input_argv, argv, &args);
        if (args != 1) {
            printf("`set height` used incorrect, only expecting 1 argument!" 
                "\n");
        } else {
            shell_config.height = atoi(argv[0]);
            printf("shell terminal height changed to %d\n", 
                shell_config.height);
        }
    }
}

int main(int argc, char *argv[])
{
    char input_buf[256];
    char *input_argv;
    char cmd[64];

    errval_t err;

    memset(input_buf, 0, 256);
    memset(cmd, 0, 64);

    shell_config.coreid = disp_get_core_id();

    printf("%s\n", pikachu_img);

    while (true) {
        shell_input(input_buf, 256);
        //printf("DEBUG: %s\n", input_buf);
        input_argv = headstr(input_buf, cmd, ' ');
        //printf("DEBUG CMD: %s\n", cmd);

        if (is_user_app(cmd)) {
            // execute spawn user application and provide arguments.
            domainid_t pid;
            err = spawn(input_buf, shell_config.coreid, &pid);
            if (err_is_fail(err)) {
                printf("Oops! Failed to spawn `%s` process\n", cmd);
            } else {
                printf("Spawned process `%s` - pid: %d\n", cmd, pid);
            }
        } else {
            if (strcmp(cmd, "echo") == 0) {
                echo (input_argv);
            } else if (strcmp(cmd, "exit") == 0) {
                printf("exiting shell... goodbye\n");
                break;
            } else if (strcmp(cmd, "run_memtest") == 0) {
                // forks a thread and runs a memory test.
                run_memtest(input_argv);
            } else if (strcmp(cmd, "oncore") == 0) {
                // eg. oncore 1 [USR_APP]
                // eg. oncore 2 [USR_APP]
                domainid_t pid;
                oncore(input_argv, &pid);
                //printf("execution on process: %d");
            } else if (strcmp(cmd, "ps") == 0) {
                ps(input_argv);
            } else if (strcmp(cmd, "kill") == 0) {
                printf("NYI!\n");
            } else if (strcmp(cmd, "fg") == 0) {
                printf("NYI!\n");
            } else if (strcmp(cmd, "jobs") == 0) {
                printf("NYI!\n");
            } else if (strcmp(cmd, "list") == 0) {
                list_app();
            } else if (strcmp(cmd, "clear") == 0) {
                clear();
            } else if (strcmp(cmd, "set") == 0 ) {
                set_shell(input_argv);
            } else if (strcmp(cmd, "pikachu") == 0) {
                printf("%s\n", pikachu_img);
            } else {
                printf("unknown command. try again\n");
            }
        }
        memset(input_buf, '\0', 256);
        memset(cmd, '\0', 64);
    }
    return 0;
}
