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
#include <string.h>
#include <barrelfish/morecore.h>
#include <barrelfish/dispatcher_arch.h>
#include <barrelfish/debug.h>
#include <barrelfish/lmp_chan.h>

struct bootinfo *bi;
static coreid_t my_core_id;

void recv_handler(void *lc_in);

int main(int argc, char *argv[])
{
    errval_t err;

    /* Set the core id in the disp_priv struct */
    err = invoke_kernel_get_core_id(cap_kernel, &my_core_id);
    assert(err_is_ok(err));
    disp_set_core_id(my_core_id);

    debug_printf("init: invoked as:");
    for (int i = 0; i < argc; i++) {
       printf(" %s", argv[i]);
    }
    printf("\n");

    debug_printf("FIRSTEP_OFFSET should be %zu + %zu\n",
            get_dispatcher_size(), offsetof(struct lmp_endpoint, k));

    // First argument contains the bootinfo location
    bi = (struct bootinfo*)strtol(argv[1], NULL, 10);

    // setup memory serving
    err = initialize_ram_alloc();
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "Failed to init local ram allocator");
        abort();
    }
    debug_printf("initialized local ram alloc\n");
    
    // setup memory serving
    err = initialize_mem_serv();
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "Failed to init memory server module");
        abort();
    }

    char *c = (char*)malloc(2);
    *c = 'c';
    
    // TODO (milestone 4): Implement a system to manage the device memory
    // that's referenced by the capability in TASKCN_SLOT_IO in the task
    // cnode. Additionally, export the functionality of that system to other
    // domains by implementing the rpc call `aos_rpc_get_dev_cap()'.
    debug_printf("initialized dev memory management\n");



    // TODO (milestone 3) STEP 2:
    // get waitset
    // TODO

    // allocate lmp chan
    struct lmp_chan lc;

    // init lmp chan
    lmp_chan_init(&lc);
   
    /* make local endpoint available -- this was minted in the kernel in a way
     * such that the buffer is directly after the dispatcher struct and the
     * buffer length corresponds DEFAULT_LMP_BUF_WORDS (excluding the kernel 
     * sentinel word).
     */
    const uint64_t FIRSTEP_BUFLEN = 21u;
    const uint64_t FIRSTEP_OFFSET = (33472u + 56u);

    struct lmp_endpoint *my_ep;
    lmp_endpoint_setup(FIRSTEP_OFFSET, FIRSTEP_BUFLEN, &my_ep);
    
    lc.endpoint = my_ep; 
    lc.local_cap = cap_initep;
    struct waitset *ws = get_default_waitset();
    lmp_chan_register_recv(&lc, ws, MKCLOSURE(recv_handler, &lc));
 
    printf("next:          0x%08x\n", my_ep->next);
    printf("prev:          0x%08x\n", my_ep->prev);
    printf("recv_slot:     0x%08x\n", my_ep->recv_slot);
    printf("waitset_state: 0x%08x\n", my_ep->waitset_state);
    printf("buflen:        0x%08x\n", my_ep->buflen);
    printf("seen:          0x%08x\n", my_ep->seen);
    printf("k:             0x%08x\n", my_ep->k);
    
    // allocate slot for incoming capabilites
    // register receive handler 
    // go into messaging main loop
    
    while(true) {
        printf("init.c waiting for event...\n");
        event_dispatch(ws);
        printf("init.c returned from handling event!\n");
    }
    
    return EXIT_SUCCESS;
}

void recv_handler(void *lc_in)
{
    struct lmp_chan *lc = lc_in;
    struct lmp_recv_msg msg = LMP_RECV_MSG_INIT;
    struct capref cap;
    errval_t err = lmp_chan_recv(lc, &msg, &cap);
    if (err_is_fail(err) && lmp_err_is_transient(err)) {
        // reregister
        lmp_chan_register_recv(lc, get_default_waitset(),
            MKCLOSURE(recv_handler, lc));
        return;
    }

    debug_printf("msg buflen %zu\n", msg.buf.msglen);
    debug_printf("msg->words[0] = 0x%lx\n", msg.words[0]);
    lmp_chan_register_recv(lc, get_default_waitset(),
        MKCLOSURE(recv_handler, lc_in));
}
