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

#define FIRSTEP_BUFLEN 21u

static void recv_cap(void *rpc_void);
static void recv_cap(void *rpc_void) 
{
    // Retrieve message with frame cap
    struct aos_rpc *rpc = (struct aos_rpc *)rpc_void;
    struct lmp_recv_msg msg = LMP_RECV_MSG_INIT;
    struct capref cap;
    errval_t err = lmp_chan_recv(&(rpc->lc), &msg, &cap);

    // If retrieval fails we just return
    if (err_is_fail(err) || capref_is_null(cap)) {
        debug_printf("Could not retrieve frame cap.\n");
        return;
    }
    
    // Store cap and return
    rpc->return_cap = cap;
}

errval_t aos_rpc_send_string(struct aos_rpc *rpc, const char *string)
{
    struct lmp_chan lc = rpc->lc;

    if (strlen(string) > 9) {
        debug_printf("aos_rpc_send_string currently does not support long strings T_T\n");
    }
    char buf[9];
    memcpy(buf, string, 9);

    errval_t err;
    err = lmp_chan_send(&lc, LMP_SEND_FLAGS_DEFAULT, NULL_CAP, 8, buf[0], 
                            buf[1], buf[2], buf[3], buf[4],
                            buf[5], buf[6], buf[7], buf[8]);

    if (err_is_fail(err)) {
        return err;
    } 

    // Dispatches to next event on given waitset. 
    err = event_dispatch(get_default_waitset());
    if (err_is_fail(err)) {
        return err;
    }
    
    return SYS_ERR_OK;
}

/**
 * FIXME This function fails when called twice in a row because init
 * does not exit its receiver function before the next msg is sent.
 */
errval_t aos_rpc_get_ram_cap(struct aos_rpc *rpc, size_t req_bits,
                             struct capref *dest, size_t *ret_bits)
{
    // Send our endpoint capability
    errval_t err = lmp_chan_send1(&(rpc->lc), LMP_SEND_FLAGS_DEFAULT,
                                  rpc->lc.local_cap, req_bits);
    
    if (err_is_fail(err)) {
        debug_printf("Could not send msg to init.\n", err);
        err_print_calltrace(err);
        exit(-1);
    }

    // Listen for response from init. When recv_cap returns,
    // cap should be in rpc->return_cap
    lmp_chan_register_recv(&rpc->lc, get_default_waitset(),
        MKCLOSURE(recv_cap, &rpc->lc));    
    event_dispatch(get_default_waitset());
    
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
    rpc->lc.local_cap = new_ep; // <-- FIXME actually needed?
    rpc->lc.remote_cap = cap_initep;
    
    // Allocate the slot for receiving
    err = lmp_chan_alloc_recv_slot(&rpc->lc);
    if (err_is_fail(err)){
        debug_printf("Could not allocate receive slot!\n");
        err_print_calltrace(err);
        exit(-1);
    }
    
    // Register our receive handler
    err = lmp_chan_register_recv(&rpc->lc, ws, MKCLOSURE(recv_cap, &rpc->lc));
    if (err_is_fail(err)){
        debug_printf("Could not register receive handler!\n");
        err_print_calltrace(err);
        return err;
    }

    // register in paging state
    struct paging_state *st = get_current_paging_state();
    st->chan = rpc;
    
    return SYS_ERR_OK;
}
