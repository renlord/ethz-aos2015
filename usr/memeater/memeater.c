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

#define BUFSIZE (128UL*1024*1024)
#define ARRAY_SIZE (1UL << 2)

struct aos_rpc rpc;

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

static void **gentle_test(void);
static void **gentle_test(void) {
    debug_printf("ARRAY_SIZE: %d, sizeof(void*): %d, sizeof(uint32_t): %d\n",
        ARRAY_SIZE, sizeof(void*), sizeof(uint32_t));
    void **array = (void **)my_malloc(ARRAY_SIZE*sizeof(void*));
    for (uint32_t i = 0; i < ARRAY_SIZE; i++) {
        uint32_t *test = (uint32_t *) my_malloc(sizeof(uint32_t));

        debug_printf("*test for i %d: 0x%08x\n", i, test);
        
        *test = i;
        array[i] = test;
    }
    return array;
}


static void check_array(void **array);
static void check_array(void **array){
    for (uint32_t i = 0; i < ARRAY_SIZE; i++) {
        void *ptr = array[i];
        uint32_t j = *((uint32_t *)ptr);
        assert(j == i);
    }
}

static void free_array(void **array);
static void free_array(void **array){
    /* TODO can't figure out how to point free()
            to morecore_free(), so for now call
            paging_dealloc() directly */

    for (uint32_t i = 0; i < ARRAY_SIZE; i++) {
        void *ptr = array[i];
        // free(ptr);
        my_free(ptr);
    }
    // free(array);
    my_free(array);
}


int main(int argc, char *argv[])
{
    debug_printf("memeater started\n");

    errval_t err;

    err = aos_rpc_init(&rpc);
    if (err_is_fail(err)){
        debug_printf("Could not initialize RPC-module\n");
        err_print_calltrace(err);
        exit(-1);
    }
    
    debug_printf("Creating array...\n");
    void **array = gentle_test();
    debug_printf("Array created with start addr 0x%08x.\n", array);
    debug_printf("First ptr: 0x%08x.\n", array[0]);
    
    debug_printf("Checking array...\n");
    check_array(array);
    debug_printf("Array checked.\n");
    
    debug_printf("Freeing array...\n");
    free_array(array);
    debug_printf("Array freed.\n");
        
    return 0;
}
