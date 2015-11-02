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
#define FIRSTEP_BUFLEN 20u

size_t client_bytes[MAX_CLIENTS];

struct bootinfo *bi;
static coreid_t my_core_id;

my_pid_t next_pid;

static char msg_buf[9];

void debug_print_mem(void);
void debug_print_mem(void){
    for (uint32_t i = 0; i < MAX_CLIENTS && client_bytes[i]; i++)
    {
        debug_printf("%d: %d\n", i, client_bytes[i]);
    }
    
}

void recv_handler(void *lc_in);
void recv_handler(void *lc_in)
{
    struct capref remote_cap = NULL_CAP;
    struct lmp_chan *lc = (struct lmp_chan *)lc_in;
    struct lmp_recv_msg msg = LMP_RECV_MSG_INIT;
    errval_t err = lmp_chan_recv(lc, &msg, &remote_cap);
    
    if (err_is_fail(err)) {
        debug_printf("Could not send msg to init: %s.\n",
            err_getstring(err));
        err_print_calltrace(err);
        return; // FIXME notify caller
    }

    if (msg.buf.msglen == 0){
        debug_printf("Bad msg for init.\n");
        return; // FIXME notify caller
    }

    lc->remote_cap = remote_cap;            

    err = lmp_chan_alloc_recv_slot(lc);
    if (err_is_fail(err)) {
        debug_printf("Could not allocate recv slot: %s.\n",
            err_getstring(err));
        err_print_calltrace(err);
        exit(-1);
    }
    
    lmp_chan_register_recv(lc, get_default_waitset(),
        MKCLOSURE(recv_handler, lc_in));
    
    uint32_t code = msg.buf.words[0];
    switch(code){
        case REQUEST_PID:
        {
            if (capref_is_null(remote_cap)) {
                debug_printf("Received endpoint cap was null.\n");
                return;
            }
    
            if (next_pid < MAX_CLIENTS){
                debug_printf("Sending pid to memeater\n");
                lmp_chan_send2(lc, LMP_SEND_FLAGS_DEFAULT, NULL_CAP,
                    REQUEST_PID, next_pid++);
            } else {
                // TODO handle
                return;
            }
            break;
        }
        
        case SEND_TEXT:
        {
            size_t buf_size = (uint32_t) msg.buf.words[1];            
            for(uint8_t i = 0; i < buf_size; i++){
                msg_buf[i] = msg.buf.words[i+2];
            }
            msg_buf[buf_size] = '\0';
            debug_printf("Received text: %s\n", msg_buf);
        }
        break;
        
        case REQUEST_FRAME_CAP:
        {
            if (capref_is_null(remote_cap)) {
                debug_printf("Received endpoint cap was null.\n");
                return;
            }
    
            my_pid_t pid = msg.buf.words[1];
            uint32_t req_bits = msg.buf.words[2];
            
            struct capref dest = NULL_CAP;
            
            // Perform the allocation
            size_t ret_bits;
            err = frame_alloc(&dest, req_bits, &ret_bits);
            if (err_is_fail(err)){
                debug_printf("Failed memserv allocation.\n");
                err_print_calltrace(err);
                return;// err; // FIXME
            }
            
            client_bytes[pid-1] += ret_bits;
            
            err = lmp_chan_send2(lc, LMP_SEND_FLAGS_DEFAULT, dest,
                REQUEST_FRAME_CAP, ret_bits);
            if (err_is_fail(err)) {
                debug_printf("Could not send msg to init: %s.\n",
                    err_getstring(err));
                err_print_calltrace(err);
                exit(-1);
            }
            break;
         }
        default:
        debug_printf("Wrong code: %d\n", code);
    }    
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

    // initialize lmp chan
    lmp_chan_init(&lc);
   
    /* make local endpoint available -- this was minted in the kernel in a way
     * such that the buffer is directly after the dispatcher struct and the
     * buffer length corresponds DEFAULT_LMP_BUF_WORDS (excluding the kernel 
     * sentinel word).
     */
    

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
    
    next_pid = 1;

    for (uint32_t i = 0; i < MAX_CLIENTS; i++) {
        client_bytes[i] = 0;
    }
    
    while(true) {
        event_dispatch(get_default_waitset());
    }

    return EXIT_SUCCESS;
}