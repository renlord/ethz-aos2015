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

// List of User Programs
#define N_APP 			4
#define OUTPUT_LEN 		256

const char *usr_app[] = { "shell", "memeater", "hello", "blink" };

static inline void list_app(void)
{	
	printf("###### AVAILABLE APPLICATIONS ######\r\n");
	for (int i = 0; i < N_APP; i++) {
		printf("%d. %s\r\n", i, usr_app[i]);
	}
	printf("###################################\r\n");
}

static inline bool is_user_app(const char *name)
{
	for (int i = 0; i < N_APP; i++) {
		if (strcmp(name, usr_app[i]) == 0) {
			return true;
		}
	}
	return false;
}

static inline void *my_malloc(size_t size)
{
    void *buf;
    paging_alloc(get_current_paging_state(), &buf, size);
    return buf;
}

static inline void my_free(void *buf)
{
    paging_dealloc(get_current_paging_state(), buf);
}

static inline char *headstr(char *str, char *head, int delim)
{
    assert(head != NULL);
    int len = strlen(str);
    int i;
    for (i = 0; i < len; i++, str++) {
        if (*str == delim)
            break;
        head[i] = str[i];
    }
    head[i] = '\0';
    return str;
}

static void **allocate_array(size_t chump_size, uint32_t array_size)
{
    void **array = (void **)my_malloc(array_size*sizeof(void*));

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
            
    printf("Creating array...\r\n");
    void **array = allocate_array(chump_size, array_size);
    printf("Array created with start addr 0x%08x.\r\n", array);
    
    debug_get_free_space(&free_s, &alloc_s, &max_s);
    printf("Free space: %d MB, allocated space: %d MB, max blob: %d MB\r\n", 
        (free_s >> 20), (alloc_s >> 20), (max_s >> 20));
    
    printf("Checking array...\r\n");
    check_array(array, array_size);
    printf("Array checked.\r\n");
    
    printf("Freeing array...\r\n");
    free_array(array, array_size);
    printf("Array freed.\r\n");
    
    debug_get_free_space(&free_s, &alloc_s, &max_s);
    printf("Free space: %d MB, allocated space: %d MB, max blob: %d MB\r\n", 
        (free_s >> 20), (alloc_s >> 20), (max_s >> 20));
}
