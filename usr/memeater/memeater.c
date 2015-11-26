#include <barrelfish/barrelfish.h>
#include <barrelfish/cspace.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <barrelfish/debug.h>
#include <barrelfish/lmp_chan_arch.h>
#include <barrelfish/lmp_chan.h>
#include <barrelfish/aos_rpc.h>
#include <barrelfish/paging.h>
#include <omap44xx_map.h>

#define SMALL_CHUMP_ARRAY_SIZE (1UL << 10)
#define SMALL_CHUMP_SIZE (1UL << 2)

#define MEDIUM_CHUMP_ARRAY_SIZE (1UL << 10)
#define MEDIUM_CHUMP_SIZE (1UL << 12)

#define BIG_CHUMP_ARRAY_SIZE (1UL << 3)
#define BIG_CHUMP_SIZE (1UL << 25)

#define BACKSPACE   8
#define ESCAPE      27
#define DELETE      127
#define SIGINT      3  
#define SIGEOF      26
#define RETURN      13
    
static void *my_malloc(size_t size);
static void *my_malloc(size_t size)
{
    void *buf;
    paging_alloc(get_current_paging_state(), &buf, size);
    return buf;
}

static void my_free(void *buf);
static void my_free(void *buf)
{
    paging_dealloc(get_current_paging_state(), buf);
}

static void **allocate_array(size_t chump_size, uint32_t array_size);
static void **allocate_array(size_t chump_size, uint32_t array_size)
{
    void **array =
        (void **)my_malloc(array_size*sizeof(void*));

    for (uint32_t i = 0; i < array_size; i++) {
        uint32_t *test = (uint32_t *) my_malloc(chump_size);
        *test = i;
        array[i] = test;
    }
    return array;
}


static void check_array(void **array, uint32_t array_size);
static void check_array(void **array, uint32_t array_size)
{
    for (uint32_t i = 0; i < array_size; i++) {
        void *ptr = array[i];
        uint32_t j = *((uint32_t *)ptr);
        assert(j == i);        
    }
}

static void free_array(void **array, uint32_t array_size);
static void free_array(void **array, uint32_t array_size)
{
    /* TODO can't figure out how to point free()
            to morecore_free(), so for now call
            paging_dealloc() directly */

    for (uint32_t i = 0; i < array_size; i++) {
        void *ptr = array[i];
        // free(ptr);
        my_free(ptr);        
    }
    // free(array);
    my_free(array);
}


static void perform_array_test(size_t chump_size, uint32_t array_size);
static void perform_array_test(size_t chump_size, uint32_t array_size)
{
    size_t free_s, alloc_s, max_s;
            
    debug_printf("Creating array...\n");
    void **array = allocate_array(chump_size, array_size);
    debug_printf("Array created with start addr 0x%08x.\n", array);
    
    debug_get_free_space(&free_s, &alloc_s, &max_s);
    debug_printf("Free space: %d MB, allocated space: %d MB, max blob: %d MB\n",
        (free_s >> 20), (alloc_s >> 20), (max_s >> 20));
    
    debug_printf("Checking array...\n");
    check_array(array, array_size);
    debug_printf("Array checked.\n");
    
    debug_printf("Freeing array...\n");
    free_array(array, array_size);
    debug_printf("Array freed.\n");
    
    debug_get_free_space(&free_s, &alloc_s, &max_s);
    debug_printf("Free space: %d MB, allocated space: %d MB, max blob: %d MB\n",
        (free_s >> 20), (alloc_s >> 20), (max_s >> 20));
}

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
        } else if(c == 127){
            i--;
            buf[i] = ' ';
            print_line("\r");
            print_line(prompt);
            print_line(buf);
            buf[i] = '\0';
            print_line("\r");
            print_line(prompt);
            print_line(buf);
        } else {
            memcpy(&buf[i], &c, 1);            
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

static void parse_cli_cmd(char *str, char **argv, int *args);
static void parse_cli_cmd(char *str, char **argv, int *args)
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

static void cli_demo(void);
static void cli_demo(void) 
{
    char input_buf[256];
    char *argv[256];
    int args, i;

    memset(input_buf, '\0', 256);

    print_line("======== BEGIN BASIC SHELL ==========\r\n");
    while (true) {
        scan_line(input_buf);
        parse_cli_cmd(input_buf, argv, &args);

        // DEBUGGING
        // debug_printf("arguments:\n");
        // for (i = 0; i < args; i++) {
        //     debug_printf("%s\n", argv[i]);
        // }

        if (strcmp(argv[0], "echo") == 0) {

            if (args < 2) {
                print_line("Insufficient arguments for echo command!\r\n");
                continue;
            }

            for (i = 1; i < args-1; i++) {
                print_line(argv[i]);
                print_line(" ");
            }
            print_line(argv[i]);
            print_line("\r\n");

        } else if (strcmp(argv[0], "run_memtest") == 0) {

            if (args != 2) {
                print_line("Insufficient argument size for run_memeater command!\r\n");
                continue;
            }

            print_line("Running Small Chump Memory Test...\r\n");
            perform_array_test(SMALL_CHUMP_SIZE, atoi(argv[1]));
            print_line("Memory Test Completed.\r\n");
            // run memeater test;
            // debug_printf("Performing small chump test...\n");
            // perform_array_test(SMALL_CHUMP_SIZE, SMALL_CHUMP_ARRAY_SIZE);
            // debug_printf("Done\n\n");

        } else if (strcmp(argv[0], "spawn") == 0) {
            domainid_t new_pid;
            errval_t err = aos_rpc_process_spawn(&local_rpc, argv[1], &new_pid);
            if(err_is_fail(err)){
                err_print_calltrace(err);
                abort();
            } // else {
 //                debug_printf("Spawned program '%s' with pid %d\n", argv[0], new_pid);
 //            }
        } else if (strcmp(argv[0], "ps") == 0) {
            domainid_t *pids;
            size_t pid_count;
            errval_t err =
                aos_rpc_process_get_all_pids(&local_rpc, &pids, &pid_count);
            
            if(err_is_fail(err)){
                err_print_calltrace(err);
                abort();
            }
            
            char count_buf[25];
            sprintf(count_buf, "Number of processes: %d", pid_count);
            print_line(count_buf);
            
            for(uint32_t j = 0; j < pid_count; j++){
                char pid_buf[8];
                sprintf(pid_buf, "%d. %d", j, pids[j]);
                print_line(pid_buf);
            }
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
    debug_printf("memeater started\n");
    debug_printf("%s, pid: %u\n", disp_name(), disp_get_domain_id());
    
    void *buf;
    paging_alloc(get_current_paging_state(), &buf, 0x60000);
    *((int *)buf) = 1;
    debug_printf("Running Command Line Interface Demo...\n");
    cli_demo();
    debug_printf("Done\n\n");

    return 0;
}
