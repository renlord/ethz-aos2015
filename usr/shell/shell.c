#include "shell.h"

#define BACKSPACE   8
#define ESCAPE      27
#define DELETE      127
#define SIGINT      3  
#define SIGEOF      26
#define RETURN      13
#define FORMFEED    12
    
static void print_line(const char *str);
static void print_line(const char *str) 
{
    size_t slen = strlen(str);
    for (size_t i = 0; i < slen; i++) {
        errval_t err = aos_rpc_serial_putchar(&local_rpc, str[i]);
        if (err_is_fail(err)) {
            debug_printf("userland printf fail! %s\n", err_getstring(err));
            err_print_calltrace(err);
            break;
        }   
    }
}

static void scan_line(char *buf);
static void scan_line(char *buf) 
{   
    char c = '\0';
    memset(buf, '\0', 256);
    size_t i = 0;
    print_line("cli-demo-shell$ ");

    while (i < 256) {
        errval_t err = aos_rpc_serial_getchar(&local_rpc, &c);
        if (err_is_fail(err)) {
            debug_printf("userland scan_line fail! %s\n", err_getstring(err));
            err_print_calltrace(err);
            break;
        }

        if (i > 0) {
            if (c == DELETE) { // Mac has no backspace T_T
                aos_rpc_serial_putchar(&local_rpc, BACKSPACE);
                c = BACKSPACE;
            }

            if (c == BACKSPACE) {
                aos_rpc_serial_putchar(&local_rpc, ' ');
                aos_rpc_serial_putchar(&local_rpc, BACKSPACE);
                i--;
            }
        }

        if (c == RETURN) {
            print_line("\r\n");
            break;
        } 

        if (c > 31 && c < 127) {
            // filtering special characters
            err = aos_rpc_serial_putchar(&local_rpc, c);
            if (err_is_fail(err)) {
                DEBUG_ERR(err, "fail to put char after getting char\n");
                err_print_calltrace(err);
            }
            memcpy(&buf[i], &c, 1);
            i++;
        }
    }
}

static void join(char *argv[], char *argv_string, uint32_t argc);
static void join(char *argv[], char *argv_string, uint32_t argc){
    uint32_t len;
    for(uint32_t i = 0, n = 0; i < argc; i++, n += len){
        sprintf(&argv_string[n], "%s ", argv[i]);
        len = strlen(argv[i]) + 1;
    }
}

static void parse_cli_cmd(char *str, char **argv, int *args);
static void parse_cli_cmd(char *str, char **argv, int *argc)
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

    *argc = i;
}

static void cli_demo(void);
static void cli_demo(void) 
{
    char input_buf[256];
    char *argv[256];
    int argc, i;

    memset(input_buf, '\0', 256);

    print_line("======== BEGIN BASIC SHELL ==========\r\n");
    while (true) {
        scan_line(input_buf);
        parse_cli_cmd(input_buf, argv, &argc);

        // DEBUGGING
        // debug_printf("arguments:\n");
        // for (i = 0; i < argc; i++) {
        //     debug_printf("%s\n", argv[i]);
        // }
        // debug_printf("input buf: >>%s<<\n", input_buf);

        if (strcmp(argv[0], "echo") == 0) {

            if (argc < 2) {
                print_line("Insufficient arguments for echo command!\r\n");
                continue;
            }

            for (i = 1; i < argc-1; i++) {
                print_line(argv[i]);
                print_line(" ");
            }
            print_line(argv[i]);
            print_line("\r\n");

        }
        else if (strcmp(argv[0], "run_memtest") == 0) {

            if (argc != 2) {
                print_line("Insufficient argument size for run_memeater command!\r\n");
                continue;
            }

            print_line("Running Small Chump Memory Test...\r\n");
            perform_array_test(SMALL_CHUMP_SIZE, atoi(argv[1]));
            print_line("Memory Test Completed.\r\n");

        }
        else if (strcmp(argv[0], "spawn") == 0) {
            domainid_t new_pid;
            
            // FIXME ugly
            char argv_str[256];
            memset(argv_str, '\0', 256);
            join(&argv[1], argv_str, argc-1);

            errval_t err = aos_rpc_process_spawn(&local_rpc, argv_str, &new_pid);
            if(err_is_fail(err)){
                err_print_calltrace(err);
                abort();
            }
        } else if (strncmp(argv[0], "pstree", 6) == 0) {
            aos_chan_send_string(&local_rpc.spawnd_lc, "pstree");
        } else if (strncmp(argv[0], "psstack", 7) == 0) {
            aos_chan_send_string(&local_rpc.spawnd_lc, "psstack");
        } else if (strncmp(argv[0], "ps", 2) == 0) {
            domainid_t *pids;
            size_t pid_count;
            errval_t err =
                aos_rpc_process_get_all_pids(&local_rpc, &pids, &pid_count);
            
            if(err_is_fail(err)){
                err_print_calltrace(err);
                abort();
            }
            
            char count_buf[25];
            const char *divide = "+--------------------------+\n\r";
            sprintf(count_buf,   "| Number of processes: %*d |\n\r",
                3, pid_count);

            print_line(divide);
            print_line(count_buf);
            print_line(divide);
            
            for(uint32_t j = 0; j < pid_count; j++){
                char *name_buf = (char *)malloc(20);
                aos_rpc_process_get_name(&local_rpc, pids[j], &name_buf);
                
                char pid_buf[32];
                sprintf(pid_buf, "| %*d. %-15s %*d |\n\r",
                    3, j, name_buf, 3, pids[j]);
                print_line(pid_buf);
            }
            print_line(divide);
        } else if (strcmp(argv[0], "kill") == 0) {
            
        } else if (strcmp(argv[0], "exit") == 0) {
            print_line("exiting shell... goodbye\r\n");
            print_line("======== END BASIC SHELL ========\r\n");
            return;
        } else {
            print_line("unknown command. try again\r\n");
        }

        memset(input_buf, '\0', 256);
    }
}

int main(int argc, char *argv[])
{
    debug_printf("shell started\n");
    debug_printf("%s, pid: %u\n", disp_name(), disp_get_domain_id());

    domainid_t pid;
    aos_rpc_process_spawn(&local_rpc, "blink", &pid);
        
    debug_printf("Running Command Line Interface Demo...\n");
    cli_demo();
    debug_printf("Done\n\n");

    return 0;
}
