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
#include <barrelfish/aos_rpc.h>


#define MAX_CLIENTS 50
struct mem_map {
    struct capref endpoint;
    uint32_t frames;
};

struct {
    struct mem_map clients[MAX_CLIENTS];
} memory_handler;


struct bootinfo *bi;
static coreid_t my_core_id;

void recv_handler(void *lc_in);
void recv_handler(void *lc_in)
{
    struct capref remote_cap;
    struct lmp_chan *lc = (struct lmp_chan *)lc_in;
    struct lmp_recv_msg msg = LMP_RECV_MSG_INIT;
    errval_t err = lmp_chan_recv(lc, &msg, &remote_cap);

    printf("msg.buf.msglen: %d\n", msg.buf.msglen);
    if (msg.buf.msglen > 1){
        exit(-1); //FIXME
    }
    
    uint32_t req_bits = *((uint32_t *)msg.buf.words[0]);
    printf("req_bits: %d\n", req_bits);

    if (err_is_fail(err) && lmp_err_is_transient(err)) {
        // reregister
        lmp_chan_register_recv(lc, get_default_waitset(),
            MKCLOSURE(recv_handler, lc));
        return; //FIXME
    }
    
    if (capref_is_null(remote_cap)) {
        return;
    }
    
    debug_printf("got a cap slot from memeater!\n");
    lc->remote_cap = remote_cap;
    
    // struct lmp_chan *lc = (struct lmp_chan *) lc_in;
    // struct lmp_recv_msg msg = LMP_RECV_MSG_INIT;
    struct capref dest = NULL_CAP;
    
    // Update mapping
    struct mem_map *m = memory_handler.clients;
    uint32_t i;
    for (i = 0; i < MAX_CLIENTS; i++, m = &memory_handler.clients[i])
    {
        if (capcmp(m->endpoint, remote_cap)){
            break;
        }
        
        if (m == NULL){
            *m = (struct mem_map){ remote_cap, 0 };
            break;
        }
    }
    
    if (i == MAX_CLIENTS){
        return;// LIB_ERR_RAM_ALLOC_MS_CONSTRAINTS;
        // FIXME
    }
    
    // Perform the allocation
    err = memserv_alloc(&dest, req_bits, 0, 0); // TODO perhaps edit max_limit
    if (err_is_fail(err)){
        return;// err; // FIXME
    }
    
    uint32_t ret_bits;
    ret_bits = req_bits; // FIXME
    
    m->frames += (uint32_t)ret_bits; // TODO divide by framesize
    
    lmp_chan_send0(lc, LMP_SEND_FLAGS_DEFAULT, dest);
    
    debug_printf("init recv_handler entered!\n");
    
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

    struct aos_rpc init_rpc = {
        .lc = &lc,
        .origin_listener = NULL_CAP,
        .n_prs = 0,
    };

    err = aos_rpc_init(&init_rpc);
    if (err_is_fail(err)) {
        debug_printf("aos_rpc_init test. err:%d\n");
    }

    while(true) {
        event_dispatch(ws);
    }

    return EXIT_SUCCESS;
}

errval_t request_memory(struct capref endpoint, struct capref *dest,
                        uint8_t req_bits, uint8_t *ret_bits);
errval_t request_memory(struct capref endpoint, struct capref *dest,
                        uint8_t req_bits, uint8_t *ret_bits)
{    
    // Check if dest is pointing anywhere
    if (dest == NULL) {
        return SYS_ERR_LMP_NO_TARGET;
    }
        
    // Update mapping
    struct mem_map *m = memory_handler.clients;
    uint32_t i;
    for (i = 0; i < MAX_CLIENTS; i++, m = &memory_handler.clients[i])
    {
        if (capcmp(m->endpoint, endpoint)){
            break;
        }
        
        if (m == NULL){
            *m = (struct mem_map){ endpoint, 0 };
            break;
        }
    }
    
    if (i == MAX_CLIENTS){
        return LIB_ERR_RAM_ALLOC_MS_CONSTRAINTS;
    }
    
    // Perform the allocation
    errval_t err = memserv_alloc(dest, req_bits, 0, 0); // TODO perhaps edit max_limit
    if (err_is_fail(err)){
        return err;
    }
    
    *ret_bits = req_bits; // FIXME
    
    m->frames += (uint32_t)ret_bits; // TODO divide by framesize
    return err;
}
