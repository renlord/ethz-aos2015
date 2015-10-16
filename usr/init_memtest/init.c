/**
 * \file
 * \brief init process.
 */

/*
 * Copyright (c) 2007, 2008, 2009, 2010, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#include "init.h"
#include <stdlib.h>
#include <barrelfish/morecore.h>
#include <barrelfish/domain.h>
#include <barrelfish/sys_debug.h>

struct bootinfo *bi;

#define MALLOC_BUFSIZE (1UL<<20)
#define BUFSIZE (48UL * 1024 * 1024) // 48MB
#define SAFE_VADDR (1UL<<25)

int main(int argc, char *argv[])
{

    printf("init: invoked as:");
    for (int i = 0; i < argc; i++) {
        printf(" %s", argv[i]);
    }
    printf("\n");

    errval_t err;

    // First argument contains the bootinfo location
    bi = (struct bootinfo*)strtol(argv[1], NULL, 10);

    /* Initialize local memory allocator */
    err = initialize_ram_alloc();
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "Failed to init ram alloc");
        abort();
    }
   
    char *static_malloc_buf = malloc(MALLOC_BUFSIZE);
    for (int i = 0; i < MALLOC_BUFSIZE; i++){
        static_malloc_buf[i] = i%255;
    }
    sys_debug_flush_cache();
    printf("static malloc buf filled\n");
    for (int i = 0; i < MALLOC_BUFSIZE; i++){
        assert(static_malloc_buf[i] == i%255);
    }
    printf("static malloc buf checked\n");

#if 0
    // extra stage to test page fault handler when paging_alloc isn't
    // implemented yet. This will most likely break your self-paging
    // implementation as it doesn't tell paging_alloc() the range of addresses
    // that are used here
    printf("testing page fault mechanism with static address %p.\n", (char*)SAFE_VADDR);
    char *cbuf = (char*)SAFE_VADDR;
    for (int i = 0; i < 16*BASE_PAGE_SIZE; i++) {
        cbuf[i] = i % 255;
    }
    sys_debug_flush_cache();
    for (int i = 0; i < 16*BASE_PAGE_SIZE; i++) {
        if (cbuf[i] != i % 255) {
            debug_printf("cbuf[%d] doesn't contain %d\n", i, i % 255);
            abort();
        }
    }
#endif

    void *vbuf;
    err = paging_alloc(get_current_paging_state(), &vbuf, BUFSIZE);
    if (err_is_fail(err)) {
        printf("error in paging_alloc: %s\n", err_getstring(err));
        abort();
    }

    if (!vbuf) {
        printf("did not get a buffer\n");
        abort();
    }

    char *buf = vbuf;

    for (int i = 0; i < BUFSIZE; i++){
        buf[i] = i%255;
    }
    printf("buf filled\n");
    sys_debug_flush_cache();
    for (int i = 0; i < BUFSIZE; i++){
        assert(buf[i] == i%255);
    }
    printf("check passed\n");

    while(1);

    return EXIT_SUCCESS;
}
