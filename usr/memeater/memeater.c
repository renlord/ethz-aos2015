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

#define MEDIUM_CHUMP_ARRAY_SIZE (1UL << 12)
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

int main(int argc, char *argv[])
{
    debug_printf("memeater started\n");

    errval_t err;

    // err = aos_rpc_init(&rpc);
    // if (err_is_fail(err)){
    //     debug_printf("Could not initialize RPC-module\n");
    //     err_print_calltrace(err);
    //     exit(-1);
    // }

    debug_printf("Performing String Test...\n");
    aos_rpc_send_string(&local_rpc, "much longer text");
    debug_printf("Done\n\n");

    debug_printf("Try to get Serial Input from init...\n");
    char c;
    err = aos_rpc_serial_getchar(&local_rpc, &c);
    if (err_is_fail(err)) {
        debug_printf("failed to get serial input from init... %s\n", 
            err_getstring(err));
        err_print_calltrace(err);
    }
    debug_printf("got this character from init ----> %c \n", c);
    debug_printf("Done\n\n");   

    debug_printf("Try to produce serial output from memeater...\n");
    err = aos_rpc_serial_putchar(&local_rpc, 'F');
    if (err_is_fail(err)) {
        debug_printf("failed to put serial output from memeater... %s\n", 
            err_getstring(err));
        err_print_calltrace(err);
    }
    debug_printf("Done\n\n");

    debug_printf("Performing Userland printf test...\n");
    print_line("hello world\n");
    debug_printf("Done\n\n");
    
    debug_printf("Performing small chump test...\n");
    perform_array_test(SMALL_CHUMP_SIZE, SMALL_CHUMP_ARRAY_SIZE);
    debug_printf("Done\n\n");

    debug_printf("Performing medium chump test...\n");
    perform_array_test(MEDIUM_CHUMP_SIZE, MEDIUM_CHUMP_ARRAY_SIZE);
    debug_printf("Done\n\n");

    debug_printf("Performing big chump test...\n");
    perform_array_test(BIG_CHUMP_SIZE, BIG_CHUMP_ARRAY_SIZE);
    debug_printf("Done\n\n");

    // debug_printf("Getting IO Cap from Init...\n");
    // placeholder paramters that do nothing.
    
    // struct capref retcap;
    // size_t retlen;
    // err = aos_rpc_get_dev_cap(&local_rpc, OMAP44XX_MAP_L4_PER_UART3, OMAP44XX_MAP_L4_PER_UART3_SIZE,
    //         &retcap, &retlen);
    
    // if (err_is_fail(err)) {
    //     debug_printf("Failed to get IO Cap from init... %s\n");
    //     err_print_calltrace(err);
    //     exit(-1);
    // }

    return 0;
}
