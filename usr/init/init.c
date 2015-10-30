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
void recv_handler(void *lc_in)
{
    debug_printf("recv_handler entered!\n");
    struct lmp_chan *lc = (struct lmp_chan *) lc_in;
    struct lmp_recv_msg msg = LMP_RECV_MSG_INIT;
    struct capref cap = NULL_CAP;
    errval_t err = lmp_chan_recv(lc, &msg, &cap);
    if (err_is_fail(err) && lmp_err_is_transient(err)) {
        // reregister
        lmp_chan_register_recv(lc, get_default_waitset(),
            MKCLOSURE(recv_handler, lc));
        return;
    }

    if (!capref_is_null(cap)) {
        debug_printf("got a cap slot from memeater!\n");
        struct lmp_endpoint *new_lmpep; //new endpoint
        err = endpoint_create(DEFAULT_LMP_BUF_WORDS, &cap, &new_lmpep);
        if (err_is_fail(err)) {
            debug_printf("failed to create endpoint. err code: %d\n", err);
        } else {
            debug_printf("endpoint to memeater created!\n");
        }
        
        lc->remote_cap = cap;
        
        char *buf = "INIT!";
        err = lmp_chan_send(lc, LMP_SEND_FLAGS_DEFAULT, NULL_CAP, 8,
                      buf[0], buf[1], buf[2], buf[3], buf[4],
                      buf[5], buf[6], buf[7], buf[8]);       
        
        thread_yield_dispatcher(cap);
        
    }

    debug_printf("msg buflen %zu\n", msg.buf.msglen);
    debug_printf("msg->words[0] = 0x%lx\n", msg.words[0]);
    lmp_chan_register_recv(lc, get_default_waitset(),
        MKCLOSURE(recv_handler, lc_in));
}

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


    // TODO (milestone 4): Implement a system to manage the device memory
    // that's referenced by the capability in TASKCN_SLOT_IO in the task
    // cnode. Additionally, export the functionality of that system to other
    // domains by implementing the rpc call `aos_rpc_get_dev_cap()'.
    debug_printf("initialized dev memory management\n");



    // TODO (milestone 3) STEP 2:
    // get waitset
    struct waitset *ws = get_default_waitset();
    waitset_init(ws);
    
    // allocate lmp chan
    struct lmp_chan lc;

    // init lmp chan
    lmp_chan_init(&lc);
   
    /* make local endpoint available -- this was minted in the kernel in a way
     * such that the buffer is directly after the dispatcher struct and the
     * buffer length corresponds DEFAULT_LMP_BUF_WORDS (excluding the kernel 
     * sentinel word).
     */
    uint32_t FIRSTEP_BUFLEN = 21u;

    struct lmp_endpoint *my_ep;
    lmp_endpoint_setup(0, FIRSTEP_BUFLEN, &my_ep);    
    
    lc.endpoint = my_ep;
    lc.local_cap = cap_selfep;
    
    // allocate slot for incoming capability from memeater
    err = lmp_chan_alloc_recv_slot(&lc);
    if (err_is_fail(err)){
        printf("Could not allocate receive slot!\n");
        exit(-1);
    }

    // register receive handler 
    err = lmp_chan_register_recv(&lc, ws, MKCLOSURE(recv_handler, &lc));
    if (err_is_fail(err)){
        printf("Could not register receive handler!\n");
        exit(-1);
    }

    while(true) {
        event_dispatch(ws);
    }

    // Part 5. Passing a Capability over LMP
    err = lmp_chan_alloc_recv_slot(&lc);
    if (err_is_fail(err)){
        printf("Could not allocate receive slot!\n");
        exit(-1);
    }

    


    return EXIT_SUCCESS;
}
