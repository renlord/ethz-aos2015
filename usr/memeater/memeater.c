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

#define SMALL_CHUMP_ARRAY_SIZE (1UL << 6)
#define SMALL_CHUMP_SIZE (1UL << 2)

#define MEDIUM_CHUMP_ARRAY_SIZE (1UL << 10)
#define MEDIUM_CHUMP_SIZE (1UL << 12)

#define BIG_CHUMP_ARRAY_SIZE (1UL << 3)
#define BIG_CHUMP_SIZE (1UL << 25)
    
static void *my_malloc(size_t size)
{
    void *buf;
    paging_alloc(get_current_paging_state(), &buf, size);
    return buf;
}

static void my_free(void *buf)
{
    paging_dealloc(get_current_paging_state(), buf);
}

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


static void check_array(void **array, uint32_t array_size)
{
    for (uint32_t i = 0; i < array_size; i++) {
        void *ptr = array[i];
        uint32_t j = *((uint32_t *)ptr);
        assert(j == i);        
    }
}

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

static void perform_array_test(size_t chump_size, uint32_t array_size)
{
    size_t free_s, alloc_s, max_s;
            
    printf("Creating array...\n");
    void **array = allocate_array(chump_size, array_size);
    printf("Array created with start addr 0x%08x.\n", array);
    
    debug_get_free_space(&free_s, &alloc_s, &max_s);
    printf("Free space: %d MB, allocated space: %d MB, max blob: %d MB\n",
        (free_s >> 20), (alloc_s >> 20), (max_s >> 20));
    
    printf("Checking array...\n");
    check_array(array, array_size);
    printf("Array checked.\n");
    
    printf("Freeing array...\n");
    free_array(array, array_size);
    printf("Array freed.\n");
    
    debug_get_free_space(&free_s, &alloc_s, &max_s);
    printf("Free space: %d MB, allocated space: %d MB, max blob: %d MB\n",
        (free_s >> 20), (alloc_s >> 20), (max_s >> 20));
}

int main(int argc, char *argv[])
{
    printf("Performing small chump test...\n");
    perform_array_test(SMALL_CHUMP_SIZE, SMALL_CHUMP_ARRAY_SIZE);
    printf("Done\n");

    // printf("Performing medium chump test...\n");
    // perform_array_test(MEDIUM_CHUMP_SIZE, MEDIUM_CHUMP_ARRAY_SIZE);
    // printf("Done\f");

    // printf("Performing big chump test...\n");
    // perform_array_test(BIG_CHUMP_SIZE, BIG_CHUMP_ARRAY_SIZE);
    // printf("Done\f");

    return 0;
}
