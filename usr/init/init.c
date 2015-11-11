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
#include <barrelfish/sys_debug.h>
#include <omap44xx_map.h>


#define MAX_CLIENTS 50
#define FIRSTEP_BUFLEN 20u
#define HARD_LIMIT (1UL << 28)

struct bootinfo *bi;
static coreid_t my_core_id;

static struct client_state *fst_client;
    
static volatile uint32_t *uart3_thr = NULL;
static volatile uint32_t *uart3_rhr = NULL;
static volatile uint32_t *uart3_fcr = NULL;
static volatile uint32_t *uart3_lsr = NULL;

struct client_state {
    struct client_state *next;
    struct lmp_endpoint *ep;
    size_t alloced;
    char mailbox[50];
    size_t char_count;
};

void recv_handler(void *lc_in);
void recv_handler(void *lc_in)
{
    struct capref remote_cap = NULL_CAP;
    struct lmp_chan *lc = (struct lmp_chan *)lc_in;
    struct lmp_recv_msg msg = LMP_RECV_MSG_INIT;

    // Retrieve msg
    errval_t err = lmp_chan_recv(lc, &msg, &remote_cap);

    if (err_is_fail(err)) {
        debug_printf("Could not retrieve message: %s.\n",
            err_getstring(err));
        err_print_calltrace(err);
        return;
    }

    // Our protocol requires that there is a procedure code
    // at a bare minimum
    if (msg.buf.msglen == 0){
        debug_printf("Bad msg for init.\n");
        return;
    }
    
    // Allocate receive struct right away for next msg
    err = lmp_chan_alloc_recv_slot(lc);
    if (err_is_fail(err)) {
        debug_printf("Could not allocate recv slot: %s.\n",
            err_getstring(err));
        err_print_calltrace(err);
        return;
    }
    
    // Re-register
    lmp_chan_register_recv(lc, get_default_waitset(),
        MKCLOSURE(recv_handler, lc_in));
    
    uint32_t code = msg.buf.words[0];
    switch(code) {
        
        // Initial request from new clients, causes init to establish
        // a new channel for future communication
        case REQUEST_CHAN:
        {
            if (capref_is_null(remote_cap)) {
                debug_printf("Received endpoint cap was null.\n");
                return;
            }
                        

            // Create new endpoint and channel, and initialize
            struct capref ep_cap;
            struct lmp_endpoint *my_ep; 
            err = endpoint_create(FIRSTEP_BUFLEN, &ep_cap, &my_ep);
            if (err_is_fail(err)) {
                debug_printf("Failed to create endpoint.\n");
                err_print_calltrace(err);
                return;
            }
            
            struct lmp_chan *new_chan =
                (struct lmp_chan *)malloc(sizeof(struct lmp_chan));
            lmp_chan_init(new_chan);

            // bootstrap new LMP channel
            new_chan->local_cap = ep_cap;
            new_chan->remote_cap = remote_cap;
            new_chan->endpoint = my_ep;


            // Allocate new client state
            struct client_state **cur = &fst_client;
            struct client_state **prev;
            while(*cur != NULL) {
                prev = cur;
                cur = &((*cur)->next);
            }
            
            struct client_state *new_state =
                (struct client_state *) malloc(sizeof(struct client_state));
            new_state->next = NULL;
            new_state->ep = my_ep;
            new_state->alloced = 0;
            new_state->char_count = 0;
            *cur = new_state;

            err = lmp_chan_alloc_recv_slot(new_chan);
            if (err_is_fail(err)) {
                debug_printf("Could not allocate receive slot!\n");
                err_print_calltrace(err);
                return;
            }

            // Register receive handler to this channel
            err = lmp_chan_register_recv(new_chan, get_default_waitset(),
                MKCLOSURE(recv_handler, new_chan));
            if (err_is_fail(err)) {
                debug_printf("Could not register receive handler\n");
                err_print_calltrace(err);
                return;
            }

            // Send new endpoint cap back to client
            err = lmp_chan_send1(new_chan, LMP_SEND_FLAGS_DEFAULT, ep_cap,
                REQUEST_CHAN);
            if (err_is_fail(err)) {
                debug_printf("Failed to send new endpoint.\n");
                err_print_calltrace(err);
                return;
            }
            
            break;
        }
        
        // Only handles messages that can be contained in a
        // single pass
        case SEND_TEXT:
        {
            
            struct client_state *cs = fst_client;
            if(cs == NULL) {
                debug_printf("Frame cap requested but no clients registered yet.\n");
                return;
            }
            
            while (cs->ep != lc->endpoint){
                cs = cs->next;
                if(cs == NULL) {
                    debug_printf("Could not find client in list\n");
                    return;
                }
            }
            
    
            for(uint8_t i = 1; i < 9; i++){
                 cs->mailbox[cs->char_count] = msg.buf.words[i];
                 cs->char_count++;
                 
                 if (msg.buf.words[i] == '\0') {
                    debug_printf("Text msg received: %s\n",
                        cs->mailbox);
                    
                    cs->char_count = 0;
                    
                    // TODO actually handle receiving a msg
                    break;
                }
            }
        }
        break;
        
        // Returns a frame capability to the client
        case REQUEST_FRAME_CAP:
        {

            if (capref_is_null(remote_cap)) {
                debug_printf("Received endpoint cap was null.\n");
                return;
            }
    
            size_t req_bits = msg.buf.words[1];
            size_t req_bytes = (1UL << req_bits);
            struct capref dest = NULL_CAP;
            
            struct client_state *cs = fst_client;
            if(cs == NULL) {
                debug_printf("Frame cap requested but no clients registered yet.\n");
                return;
            }
            
            while (cs->ep != lc->endpoint){
                cs = cs->next;
                if(cs == NULL) {
                    debug_printf("Could not find client in list\n");
                    return;
                }
            }
            
            if (cs->alloced + req_bytes >= HARD_LIMIT){
                debug_printf("Client request exceeds hard limit.\n");
                return;
            }
            
            // Perform the allocation
            size_t ret_bits, ret_bytes;
            err = frame_alloc(&dest, req_bytes, &ret_bytes);
            if (err_is_fail(err)){
                debug_printf("Failed memserv allocation.\n");
                err_print_calltrace(err);
            }
            
            cs->alloced += ret_bytes;
            
            // Send cap and return bits back to caller
            ret_bits = log2ceil(ret_bytes);
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
        
        case REQUEST_DEV_CAP:
        {
            if (capref_is_null(remote_cap)) {
                debug_printf("Received endpoint cap was null.\n");
                return;
            }
            
            //err = devframe_type(&dest, cap_io, cap_io.cnode.size_bits);
            // allocate a frame for vnode
            if (err_is_fail(err)) {
                debug_printf("Could not copy TASKCN_SLOT_IO Capability. %s\n", err);
                err_print_calltrace(err);
                return;
            }

            err = lmp_chan_send1(lc, LMP_SEND_FLAGS_DEFAULT, cap_io, REQUEST_DEV_CAP);

            if (err_is_fail(err)) {
                debug_printf("failed to send copy of cap io. %s\n", err);
                err_push(err, LIB_ERR_LMP_CHAN_SEND);
                return;
            }

            break;
        }

        default:
        {
            // include LIB_ERR_NOT_IMPLEMENTED? 
            debug_printf("Wrong code: %d\n", code);
        }
    }    
}

void set_uart3_registers(lvaddr_t base);
void set_uart3_registers(lvaddr_t base)
{
    uart3_thr = (uint32_t *)((uint32_t)base + 0x0000);
    uart3_rhr = (uint32_t *)((uint32_t)base + 0x0000);
    uart3_fcr = (uint32_t *)((uint32_t)base + 0x0008);
    uart3_lsr = (uint32_t *)((uint32_t)base + 0x0014);
}

void my_print(const char *buf);
void my_print(const char *buf)
{
    assert(uart3_lsr && uart3_thr);
    for (uint32_t i = 0; i < strlen(buf); i++){
        while(*uart3_lsr == 0x20);
        // FIXME why not this? --> while((*uart3_lsr & 0x20) != 0); 
        *uart3_thr = buf[i];
    }
}

void my_read(void);
void my_read(void)
{
    char buf[256];
    memset(buf, '\0', 256);

    size_t i = 0;

    while (true) {
        *uart3_fcr &= ~1; // write 0 to bit 0
        // *uart3_fcr |= 2; // write 1 to bit 1
        *((uint8_t *)uart3_rhr) = 0;
        while ((*uart3_lsr & 1) == 0);
        my_print((char *)uart3_rhr);
        memcpy(&buf[i++], (char *) uart3_rhr, 1);
        if (*uart3_rhr == 13) { // ENTER keystroke.
            debug_printf("cur buf: %s\n", buf);
            printf("\n");
            i = 0;
        }
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
    debug_printf("Initialized dev memory management\n");
        
    debug_printf("Allocating cap_io\n");
    void *buf;
    int flags = VREGION_FLAGS_READ_WRITE | KPI_PAGING_FLAGS_NOCACHE;
    err = paging_map_frame_attr(get_current_paging_state(), &buf, (1UL<<30),
                                cap_io, flags, NULL, NULL);
    debug_printf("Done.\n");

    if (err_is_fail(err)) {
        debug_printf("Could not map io cap: %s\n", err_getstring(err));
        err_print_calltrace(err);
        abort();
    }
    
    const uint32_t base_io = 0x40000000;
    const uint32_t uart3_vaddr =
        (OMAP44XX_MAP_L4_PER_UART3-base_io)+(lvaddr_t)buf;

    set_uart3_registers(uart3_vaddr);
    
    //my_print("mic check, 1!\n");
    //my_read();

    struct thread *input_reader = thread_create((thread_func_t) my_read, NULL);
    err = thread_detach(input_reader);

    if (err_is_fail(err)) {
        debug_printf("Failed to detach Input Reading Thread. %s\n", err_getstring(err));
        err_print_calltrace(err);
    }
        
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
    
    fst_client = (struct client_state *) malloc(sizeof(struct client_state));

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
    
    debug_printf("Entering dispatch loop\n");
    while(true) {
        event_dispatch(get_default_waitset());
    }

    return EXIT_SUCCESS;
}
