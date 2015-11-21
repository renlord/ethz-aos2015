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
    size_t i;
    print_line("cli-demo-shell$ ");
    for (i = 0; i < 256; i++) {
        errval_t err = aos_rpc_serial_getchar(&local_rpc, &c);
        if (err_is_fail(err)) {
            debug_printf("userland scan_line fail! %s\n", err_getstring(err));
            err_print_calltrace(err);
            break;
        }
        
        if (c == 13) {
            print_line("\r\n");
            break;
        }
        memcpy(&buf[i], &c, 1);
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
    debug_printf("memeaters domain_name: %s\n", disp_name());
    debug_printf("memeaters domain_id: %d\n", disp_get_domain_id());
    debug_printf("Performing String Test...\n");
    // for(uint32_t i = 0; i < 150; i++){
    //     char b[4];
    //     sprintf(b, "long long long long long string no %d", i);
    //     aos_rpc_send_string(&local_rpc, b);
    // }
    // debug_printf("Done\n\n");

    //debug_printf("Performing Userland scanf test...\n");
    //char buf[256];
    //debug_printf("Type something here now: \n\n");
    //scan_line(buf);
    //debug_printf("scanned line was -----> %s\n", buf);
    //debug_printf("Done\n\n");
    
    //debug_printf("      ning Command Line Interface Demo...\n");
    //cli_demo();
    //debug_printf("Done\n\n");

    // debug_printf("Performing small chump test...\n");
    // perform_array_test(SMALL_CHUMP_SIZE, SMALL_CHUMP_ARRAY_SIZE);
    // debug_printf("Done\n\n");

    debug_printf("Getting IO Cap from Init...\n");
    errval_t err;
    struct capref retcap;
    size_t retlen;
    err = aos_rpc_get_dev_cap(&local_rpc, OMAP44XX_MAP_L4_PER_UART3,
        OMAP44XX_MAP_L4_PER_UART3_SIZE, &retcap, &retlen);

    if (err_is_fail(err)) {
        debug_printf("Failed to get IO Cap from init... %s\n");
        err_print_calltrace(err);
        exit(-1);
    }

    // debug_printf("test process_get_name.\n");
    // char *name;
    // err = aos_rpc_process_get_name(&local_rpc, disp_get_domain_id(), &name);
    // if (err_is_fail(err)) {
    //     debug_printf("Failed to spawn from memeater... %s\n");
    //     err_print_calltrace(err);
    //     exit(-1);
    // }
    // debug_printf("name of process: %s\n", name);
    // debug_printf("done\n\n");
    //
    // debug_printf("spawn blink from memeater.\n");
    // domainid_t pid;
    // err = aos_rpc_process_spawn(&local_rpc, "blink\0", &pid);
    // if (err_is_fail(err)) {
    //     debug_printf("Failed to spawn from memeater... %s\n");
    //     err_print_calltrace(err);
    //     exit(-1);
    // }
    // debug_printf("done\n\n");

    // debug_printf("Running Command Line Interface Demo...\n");
    // cli_demo();
    // debug_printf("Done\n\n");

    
    for(uint32_t i = 0; i < 100; i++){
        aos_rpc_send_string(&local_rpc, "ping");
        event_dispatch(get_default_waitset());
    }
    
    // debug_printf("Test get all pids...\n");
    // domainid_t *arr; 
    // size_t pid_count;
    // err = aos_rpc_process_get_all_pids(&local_rpc, &arr, &pid_count);
    // if (err_is_fail(err)) {
    //     debug_printf("Failed to get all pids from init... %s\n");
    //     err_print_calltrace(err);
    //     exit(-1);
    // }

    // for (size_t i = 0; i < pid_count; i++) {
    //     debug_printf("pid: %d\n", arr[i]);
    // }
    // debug_printf("Done.\n");

    debug_printf("Running Command Line Interface Demo...\n");
    cli_demo();
    debug_printf("Done\n\n");

    // debug_printf("Performing small chump test...\n");
    // perform_array_test(SMALL_CHUMP_SIZE, SMALL_CHUMP_ARRAY_SIZE);
    // debug_printf("Done\n\n");

    // debug_printf("Performing medium chump test...\n");
    // perform_array_test(MEDIUM_CHUMP_SIZE, MEDIUM_CHUMP_ARRAY_SIZE);
    // debug_printf("Done\n\n");

    // debug_printf("Performing big chump test...\n");
    // perform_array_test(BIG_CHUMP_SIZE, BIG_CHUMP_ARRAY_SIZE);
    // debug_printf("Done\n\n");

    //
    // debug_printf("Done\n\n");
        
    return 0;
}
