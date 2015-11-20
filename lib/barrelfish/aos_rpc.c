/**
 * \file
 * \brief Implementation of AOS rpc-like messaging
 */

/*
 * Copyright (c) 2013, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#include <barrelfish/aos_rpc.h>
#include <barrelfish/paging.h>
#include <barrelfish/barrelfish.h>
#include <barrelfish/cspace.h>

#define FIRSTEP_BUFLEN 20u
#define MIN(a,b) \
    ({ __typeof__ (a) _a = (a); \
     __typeof__ (b) _b = (b); \
     _a < _b ? _a : _b; })

static void clean_aos_rpc_msgbuf(struct aos_rpc *rpc);

static void recv_handler(void *rpc_void);
static void recv_handler(void *rpc_void) 
{
    struct aos_rpc *rpc = (struct aos_rpc *)rpc_void;
    struct capref remote_cap = NULL_CAP;
    struct lmp_recv_msg msg = LMP_RECV_MSG_INIT;
    errval_t err = lmp_chan_recv(&rpc->lc, &msg, &remote_cap);
    
    if (err_is_fail(err)) {
        debug_printf("Could not receive msg from init: %s\n",
            err_getstring(err));
        err_print_calltrace(err);
        rpc->return_cap = NULL_CAP;
        return;
    }

    if (msg.buf.msglen == 0){
        debug_printf("Bad msg for init.\n");
        rpc->return_cap = NULL_CAP;
        return;
    }
    
    uint32_t code = msg.buf.words[0];
    switch(code) {
        case REQUEST_CHAN:
        {
            if (capref_is_null(remote_cap)) {
                debug_printf("Received endpoint cap was null.\n");
                return;
            }

            rpc->lc.remote_cap = remote_cap;
            break;
        }
        
        case SEND_TEXT:
        {
            for(uint8_t i = 1; i < 9; i++){
                 rpc->msg_buf[rpc->char_count] = msg.buf.words[i];
                 rpc->char_count++;
                 
                 if (msg.buf.words[i] == '\0') {
                    debug_printf("Text msg received: %s\n", rpc->msg_buf);
                    rpc->char_count = 0;
                    break;
                }
            }
            
            break;
        }
        
        case REQUEST_FRAME_CAP:
        {
            if (capref_is_null(remote_cap)) {
                debug_printf("Remote_cap is NULL\n");
                return;
            }
            
            rpc->return_cap = remote_cap;
            rpc->ret_bits = msg.buf.words[1];
            break;
        }

        case REQUEST_DEV_CAP:
        {
            if (capref_is_null(remote_cap)) {
                debug_printf("remote_cap is NULL\n");
                return;
            }

            rpc->return_cap = remote_cap;
            rpc->ret_bits = msg.buf.words[1];
            break;
        }

        case SERIAL_PUT_CHAR:
        {
            // Init will never reply to this event!
            break;
        }

        case SERIAL_GET_CHAR:
        {
            if (msg.buf.msglen != 2){
                debug_printf("Bad msg for SERIAL_GET_CHAR.\n");
                rpc->return_cap = NULL_CAP;
                return;
            }
            memcpy(&rpc->msg_buf, (char *) &msg.buf.words[1], 1);
            // empty buffer
            memset(msg.buf.words, '\0', 256);            
            break;
        }

        case PROCESS_SPAWN:
        {
            if (msg.buf.msglen != 2){
                debug_printf("Bad msg for PROCESS_SPAWN.\n");
                rpc->return_cap = NULL_CAP;
                return;
            }   

            // expecting a pid to be returned and success/failure
            rpc->msg_buf[0] = msg.buf.words[1];
            if (msg.buf.words[1] == true) {
                rpc->msg_buf[1] = msg.buf.words[2];
            }

            break;
        }

        case PROCESS_GET_NAME:
        {
            // if this event is triggered, check the buffer for the full
            // text.
            rpc->wait_event = false;
            break;
        }

        case PROCESS_GET_ALL_PIDS:
        {
            // get number of pids
            rpc->msg_buf[0] = msg.buf.words[1];
            break;
        }

        default:
        {
            debug_printf("Wrong rpc code!\n");

        }
    }

    // Re-allocate
    if (!capref_is_null(remote_cap)){
        err = lmp_chan_alloc_recv_slot(&rpc->lc);
        if (err_is_fail(err)){
            debug_printf("Could not allocate receive slot: %s.\n",
                err_getstring(err));
            err_print_calltrace(err);
            return;
        }
    }
    
    // Register our receive handler
    err = lmp_chan_register_recv(&rpc->lc, get_default_waitset(), 
        MKCLOSURE(recv_handler, rpc));
    if (err_is_fail(err)){
        debug_printf("Could not register receive handler!\n");
        err_print_calltrace(err);
        return;
    }
}

errval_t aos_rpc_send_string(struct aos_rpc *rpc, const char *string)
{
    struct lmp_chan lc = rpc->lc;

    size_t slen = strlen(string) + 1; // adjust for null-character
    size_t rlen = 0;
    char buf[8];
    errval_t err;
    
    while (rlen < slen) {
        size_t chunk_size = MIN(slen-rlen, 8);
        memcpy(buf, string, chunk_size);
        err = lmp_chan_send(&lc, LMP_SEND_FLAGS_DEFAULT, NULL_CAP,
                            9, SEND_TEXT, buf[0], buf[1], buf[2],
                            buf[3], buf[4], buf[5], buf[6], buf[7]);

        if (err_is_fail(err)) {
            return err;
        } 

        string = &(string[8]);
        rlen += 8;

    }

    return SYS_ERR_OK;
}


errval_t aos_rpc_get_ram_cap(struct aos_rpc *rpc, size_t req_bits,
                             struct capref *dest, size_t *ret_bits)
{   
    // Allocate receive slot
    errval_t err = lmp_chan_alloc_recv_slot(&rpc->lc);
    if (err_is_fail(err)){
        debug_printf("Could not allocate receive slot: %s.\n",
            err_getstring(err));
        err_print_calltrace(err);
        return err_push(err, LIB_ERR_LMP_ALLOC_RECV_SLOT);
    }
    
    // Request frame cap from init
    err = lmp_chan_send2(&rpc->lc, LMP_SEND_FLAGS_DEFAULT,
                         NULL_CAP, REQUEST_FRAME_CAP,
                         req_bits); // TODO transfer error code
    
    if (err_is_fail(err)) {
        debug_printf("Could not send msg to init: %s.\n",
            err_getstring(err));
        err_print_calltrace(err);
        return err_push(err, LIB_ERR_LMP_CHAN_SEND);
    }

    // Listen for response from init. When recv_handler returns,
    // cap should be in rpc->return_cap
    event_dispatch(get_default_waitset());
    *dest = rpc->return_cap;
    *ret_bits = rpc->ret_bits;
    
    return SYS_ERR_OK;
}

errval_t aos_rpc_get_dev_cap(struct aos_rpc *rpc, lpaddr_t paddr,
                             size_t length, struct capref *retcap,
                             size_t *retlen)
{
    // TODO (milestone 4): implement functionality to request device memory
    // capability.
    
    // Allocate recieve slot
    errval_t err = lmp_chan_alloc_recv_slot(&rpc->lc);
    if (err_is_fail(err)){
        debug_printf("Could not allocate receive slot: %s.\n",
            err_getstring(err));
        err_print_calltrace(err);
        return err_push(err, LIB_ERR_LMP_ALLOC_RECV_SLOT);
    }

    // Send request to init
    err = lmp_chan_send3(&rpc->lc, LMP_SEND_FLAGS_DEFAULT,
                         NULL_CAP, REQUEST_DEV_CAP, paddr,
                         length);
    
    if (err_is_fail(err)) {
        debug_printf("Could not send dev cap request to init: %s.\n",
            err_getstring(err));
        err_print_calltrace(err);
        return err_push(err, LIB_ERR_LMP_CHAN_SEND);
    }

    // Listen for response from init. When recv_handler returns,
    // cap should be in rpc->return_cap
    event_dispatch(get_default_waitset());

    /* MEMORY STUFF HAPPENS HERE */

    // allocate space in Virtual Memory for the Device Memory
    // void *va = //malloc(length);
    // void *va = (void *)(1UL<<30);

    // // we then compute the slot offset from the base page table using the virtual address
    // // provided.

    // // assert that the requested device physical address is greater than 0x40000000
    // assert(paddr > 0x40000000); 
    
    // uint64_t start = (uint64_t) (paddr - 0x40000000);
    
    // err = paging_map_user_device(get_current_paging_state(), (lvaddr_t) va, 
    //     rpc->return_cap, start, length, VREGION_FLAGS_READ_WRITE);

    // if (err_is_fail(err)) {
    //     debug_printf("failed to map memory device to local virtual"
    //         " memory. %s\n", err_getstring(err));
    //     err_print_calltrace(err);
    // }

    *retcap = rpc->return_cap;
    *retlen = rpc->ret_bits;
    
    return SYS_ERR_OK;
}

errval_t aos_rpc_serial_getchar(struct aos_rpc *chan, char *retc)
{
    // TODO (milestone 4): implement functionality to request a character from
    // the serial driver.
    struct lmp_chan lc = chan->lc;

    errval_t err;

    err = lmp_chan_send1(&lc, LMP_SEND_FLAGS_DEFAULT, NULL_CAP, 
        SERIAL_GET_CHAR);

    if (err_is_fail(err)) {
        debug_printf("failed to get serial input from init. %s\n", 
            err_getstring(err));
        err_print_calltrace(err);
        return err;
    }

    // listen for response from init. When recv_handler returns,
    // character should be in chan->msg_buf[2]
    event_dispatch(get_default_waitset());
    
    debug_printf("aos_rpc_serial_putchar pre\n");
    *retc = chan->msg_buf[0];
    debug_printf("aos_rpc_serial_putchar post\n");
    return SYS_ERR_OK;
}


errval_t aos_rpc_serial_putchar(struct aos_rpc *chan, char c)
{
    // TODO (milestone 4): implement functionality to send a character to the
    // serial port.
    struct lmp_chan lc = chan->lc;
    errval_t err;
    
    err = lmp_chan_send2(&lc, LMP_SEND_FLAGS_DEFAULT, NULL_CAP, 
        SERIAL_PUT_CHAR, c);
    if (err_is_fail(err)) {
        return err;
    } 

    return SYS_ERR_OK;
}

errval_t aos_rpc_process_spawn(struct aos_rpc *chan, char *name,
                               domainid_t *newpid)
{
    // TODO (milestone 5): implement spawn new process rpc
    struct lmp_chan lc = chan->lc; 
    errval_t err;

    err = aos_rpc_send_string(chan, name);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "fail to send process name to init.\n");
        return err;
    }
    err = lmp_chan_send1(&lc, LMP_SEND_FLAGS_DEFAULT, NULL_CAP, 
        PROCESS_SPAWN);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "fail to send PROCESS_SPAWN event to init.\n");
        return err;
    }

    event_dispatch(get_default_waitset());

    if (chan->msg_buf[0] == true) {
        *newpid = chan->msg_buf[1];
    } else {
        debug_printf("spawn failed.\n");
    }

    clean_aos_rpc_msgbuf(chan);

    return SYS_ERR_OK;
}

errval_t aos_rpc_process_get_name(struct aos_rpc *chan, domainid_t pid,
                                  char **name)
{
    // TODO (milestone 5): implement name lookup for process given a process
    // id
    struct lmp_chan lc = chan->lc; 
    errval_t err;

    err = lmp_chan_send2(&lc, LMP_SEND_FLAGS_DEFAULT, NULL_CAP, 
        PROCESS_GET_NAME, pid);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "fail to send PROCESS_GET_NAME event to init.\n");
        return err;
    }

    chan->wait_event = true;
    while (chan->wait_event) {
        event_dispatch(get_default_waitset());
    }
    memcpy(chan->process_name, chan->msg_buf,
        strlen((char *)chan->msg_buf));
    *name = chan->process_name;

    clean_aos_rpc_msgbuf(chan);

    return SYS_ERR_OK;
}

errval_t aos_rpc_process_get_all_pids(struct aos_rpc *chan,
                                      domainid_t **pids, size_t *pid_count)
{
    // TODO (milestone 5): implement process id discovery

    struct lmp_chan lc = chan->lc; 
    errval_t err;

    err = lmp_chan_send1(&lc, LMP_SEND_FLAGS_DEFAULT, NULL_CAP, 
        PROCESS_GET_ALL_PIDS);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "fail to send PROCESS_GET_ALL_PIDS event to init.\n");
        return err;
    }

    // 1. get pid_count
    event_dispatch(get_default_waitset());
    *pid_count = chan->msg_buf[0];
    clean_aos_rpc_msgbuf(chan);

    *pids = (domainid_t *) malloc(*pid_count * sizeof(domainid_t));

    // 2. for each pid, we dispatch event.
    for (size_t i = 0; i < *pid_count; i++) {
        event_dispatch(get_default_waitset());
        *pids[i] = chan->msg_buf[0];
        clean_aos_rpc_msgbuf(chan);
    }

    return SYS_ERR_OK;
}

errval_t aos_rpc_open(struct aos_rpc *chan, char *path, int *fd)
{
    // TODO (milestone 7): implement file open
    return SYS_ERR_OK;
}

errval_t aos_rpc_readdir(struct aos_rpc *chan, char* path,
                         struct aos_dirent **dir, size_t *elem_count)
{
    // TODO (milestone 7): implement readdir
    return SYS_ERR_OK;
}

errval_t aos_rpc_read(struct aos_rpc *chan, int fd, size_t position, size_t size,
                      void** buf, size_t *buflen)
{
    // TODO (milestone 7): implement file read
    return SYS_ERR_OK;
}

errval_t aos_rpc_close(struct aos_rpc *chan, int fd)
{
    // TODO (milestone 7): implement file close
    return SYS_ERR_OK;
}

errval_t aos_rpc_write(struct aos_rpc *chan, int fd, size_t position, size_t *size,
                       void *buf, size_t buflen)
{
    // TODO (milestone 7): implement file write
    return SYS_ERR_OK;
}

errval_t aos_rpc_create(struct aos_rpc *chan, char *path, int *fd)
{
    // TODO (milestone 7): implement file create
    return SYS_ERR_OK;
}

errval_t aos_rpc_delete(struct aos_rpc *chan, char *path)
{
    // TODO (milestone 7): implement file delete
    return SYS_ERR_OK;
}

/**
 * The caller should configure
 * the right LMP Channel configs prior to calling this init
 * function.
 */
errval_t aos_rpc_init(struct aos_rpc *rpc)
{
    // Get waitset for memeater
    struct waitset *ws = get_default_waitset();
    waitset_init(ws);

    // Initialize LMP channel
    lmp_chan_init(&(rpc->lc));

    rpc->char_count = 0;
    
    // Setup endpoint and allocate associated capability
    struct capref ep_cap;
    struct lmp_endpoint *my_ep;
    
    errval_t err = endpoint_create(FIRSTEP_BUFLEN, &ep_cap, &my_ep);

    if(err_is_fail(err)){
        debug_printf("Could not allocate new endpoint.\n");
        err_print_calltrace(err);
        return err;
    }

    // Set relevant members of LMP channel
    rpc->lc.endpoint = my_ep;
    rpc->lc.local_cap = ep_cap;
    rpc->lc.remote_cap = cap_initep;
    
    // Allocate the slot for receiving
    err = lmp_chan_alloc_recv_slot(&rpc->lc);
    if (err_is_fail(err)){
        debug_printf("Could not allocate receive slot!\n");
        err_print_calltrace(err);
        return err;
    }
    
    // Register our receive handler
    err = lmp_chan_register_recv(&rpc->lc, ws, MKCLOSURE(recv_handler, rpc));
    if (err_is_fail(err)){
        debug_printf("Could not register receive handler!\n");
        err_print_calltrace(err);
        return err;
    }
    
    // Request pid from init
    // for some reason, could not see that initep is attached to init's dispatcher? 
    err = lmp_chan_send1(&(rpc->lc), LMP_SEND_FLAGS_DEFAULT,
                        rpc->lc.local_cap, REQUEST_CHAN);
    if (err_is_fail(err)) {
        debug_printf("Could not send msg to init: %s.\n",
            err_getstring(err));
        err_print_calltrace(err);
        return err;
    }

    // register in paging state
    struct paging_state *st = get_current_paging_state();
    
    st->rpc = rpc;

    // Listen for response from init. When recv_handler returns,
    // cap should be in rpc->return_cap
    debug_printf("Waiting for response from init\n");
    event_dispatch(get_default_waitset());

    return SYS_ERR_OK;
}

static void clean_aos_rpc_msgbuf(struct aos_rpc *rpc)
{
    memset(rpc->msg_buf, '\0', AOS_RPC_MSGBUF_LEN);
}
