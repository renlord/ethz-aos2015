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

#define FIRSTEP_BUFLEN 20u

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
        exit(-1); // FIXME notify caller
    }

    if (msg.buf.msglen == 0){
        debug_printf("Bad msg for init.\n");
        exit(-1); // FIXME notify caller
    }
    
    uint32_t code = msg.buf.words[0];
    switch(code) {
        case REQUEST_PID:
        {
            rpc->pid = msg.buf.words[1];
            debug_printf("Received pid: %d\n", rpc->pid);
        }
        break;
        
        case SEND_TEXT:
        {
            size_t buf_size = (uint32_t) msg.buf.words[1];
            memcpy(rpc->msg_buf, (char*)msg.buf.words[2], buf_size);
            rpc->msg_buf[buf_size] = '\0';
            debug_printf("Received text: %s\n", rpc->msg_buf);
        }
        break;
        
        case REQUEST_FRAME_CAP:
        {
            if (capref_is_null(remote_cap)) {
                debug_printf("Remote_cap is NULL\n");
                exit(-1);
            }
            
            rpc->return_cap = remote_cap;
            rpc->ret_bits = msg.buf.words[1];
        }
        break;

        default:
        debug_printf("Wrong rpc code!\n");
    }
    
    err = lmp_chan_alloc_recv_slot(&rpc->lc);
    if (err_is_fail(err)){
        debug_printf("Could not allocate receive slot: %s.\n",
            err_getstring(err));
        err_print_calltrace(err);
        exit(-1);
    }
    
    // Register our receive handler
    err = lmp_chan_register_recv(&rpc->lc, get_default_waitset(), 
        MKCLOSURE(recv_handler, rpc));
    if (err_is_fail(err)){
        debug_printf("Could not register receive handler!\n");
        err_print_calltrace(err);
        exit(-1);
    }
}

errval_t aos_rpc_send_string(struct aos_rpc *rpc, const char *string)
{
    struct lmp_chan lc = rpc->lc;

    if (strlen(string) > 7) {
        debug_printf("aos_rpc_send_string currently does not support long strings T_T\n");
    }
    char buf[7];
    memcpy(buf, string, 7);

    errval_t err;
    err = lmp_chan_send(&lc, LMP_SEND_FLAGS_DEFAULT, NULL_CAP,
                        9, SEND_TEXT, 7, buf[0], buf[1], buf[2],
                        buf[3], buf[4], buf[5], buf[6]);

    if (err_is_fail(err)) {
        return err;
    } 

    return SYS_ERR_OK;
}


errval_t aos_rpc_get_ram_cap(struct aos_rpc *rpc, size_t req_bits,
                             struct capref *dest, size_t *ret_bits)
{    
    // Send our endpoint capability
    errval_t err = lmp_chan_alloc_recv_slot(&rpc->lc);
    if (err_is_fail(err)){
        debug_printf("Could not allocate receive slot: %s.\n",
            err_getstring(err));
        err_print_calltrace(err);
        exit(-1);
    }
    
    err = lmp_chan_send3(&rpc->lc, LMP_SEND_FLAGS_DEFAULT,
                         rpc->lc.local_cap, REQUEST_FRAME_CAP,
                         rpc->pid, req_bits);
    
    if (err_is_fail(err)) {
        debug_printf("Could not send msg to init: %s.\n",
            err_getstring(err));
        err_print_calltrace(err);
        exit(-1);
    }

    // Listen for response from init. When recv_handler returns,
    // cap should be in rpc->return_cap
    event_dispatch(get_default_waitset());
    *dest = rpc->return_cap;
    *ret_bits = rpc->ret_bits;
    
    return err;
}


errval_t aos_rpc_get_dev_cap(struct aos_rpc *chan, lpaddr_t paddr,
                             size_t length, struct capref *retcap,
                             size_t *retlen)
{
    // TODO (milestone 4): implement functionality to request device memory
    // capability.
    return SYS_ERR_OK;
}

errval_t aos_rpc_serial_getchar(struct aos_rpc *chan, char *retc)
{
    // TODO (milestone 4): implement functionality to request a character from
    // the serial driver.
    return SYS_ERR_OK;
}


errval_t aos_rpc_serial_putchar(struct aos_rpc *chan, char c)
{
    // TODO (milestone 4): implement functionality to send a character to the
    // serial port.
    return SYS_ERR_OK;
}

errval_t aos_rpc_process_spawn(struct aos_rpc *chan, char *name,
                               domainid_t *newpid)
{
    // TODO (milestone 5): implement spawn new process rpc
    return SYS_ERR_OK;
}

errval_t aos_rpc_process_get_name(struct aos_rpc *chan, domainid_t pid,
                                  char **name)
{
    // TODO (milestone 5): implement name lookup for process given a process
    // id
    return SYS_ERR_OK;
}

errval_t aos_rpc_process_get_all_pids(struct aos_rpc *chan,
                                      domainid_t **pids, size_t *pid_count)
{
    // TODO (milestone 5): implement process id discovery
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
    // const uint64_t FIRSTEP_BUFLEN = 21u;
    // Get waitset for memeater
    struct waitset *ws = get_default_waitset();
    waitset_init(ws);


    // Initialize LMP channel
    lmp_chan_init(&(rpc->lc));
    
    // Setup endpoint and allocate associated capability
    struct capref new_ep;
    struct lmp_endpoint *my_ep;
    

    errval_t err = endpoint_create(FIRSTEP_BUFLEN, &new_ep, &my_ep);

    if(err_is_fail(err)){
        debug_printf("Could not allocate new endpoint.\n");
        err_print_calltrace(err);
        return err;
    }

    // Set relevant members of LMP channel
    rpc->lc.endpoint = my_ep;
    rpc->lc.local_cap = new_ep;
    rpc->lc.remote_cap = cap_initep;
    
    // Allocate the slot for receiving
    err = lmp_chan_alloc_recv_slot(&rpc->lc);
    if (err_is_fail(err)){
        debug_printf("Could not allocate receive slot!\n");
        err_print_calltrace(err);
        exit(-1);
    }
    
    // Register our receive handler
    err = lmp_chan_register_recv(&rpc->lc, ws, MKCLOSURE(recv_handler, rpc));
    if (err_is_fail(err)){
        debug_printf("Could not register receive handler!\n");
        err_print_calltrace(err);
        return err;
    }
    
    // Request pid from init
    err = lmp_chan_send1(&(rpc->lc), LMP_SEND_FLAGS_DEFAULT,
                         rpc->lc.local_cap, REQUEST_PID);
    if (err_is_fail(err)) {
        debug_printf("Could not send msg to init: %s.\n",
            err_getstring(err));
        err_print_calltrace(err);
        exit(-1);
    } else {
        debug_printf("Sent request to init.c\n");
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
