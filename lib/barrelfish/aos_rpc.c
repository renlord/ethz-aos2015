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

void recv_handler(void *lc_in);

void recv_handler(void *lc_in) 
{
    struct lmp_chan *lc = (struct lmp_chan *) lc_in;
    struct lmp_recv_msg msg = LMP_RECV_MSG_INIT;
    struct capref cap;
    errval_t err = lmp_chan_recv(lc, &msg, &cap);
    if (err_is_fail(err) && lmp_err_is_transient(err)) {
        lmp_chan_register_recv(lc, get_default_waitset(), 
            MKCLOSURE(recv_handler, lc));
        return;
    }

    if (!capref_is_null(&cap)) {
        err = aos_rpc_get_ram_cap(lc, atoi(msg->buf), &cap, NULL);
        if (err_is_fail(err)) {
            debug_printf("fail to call get ram\n");
        }      
    }
}

errval_t aos_rpc_send_string(struct aos_rpc *chan, const char *string)
{
    // TODO: implement functionality to send a string over the given channel
    // and wait for a response.
    struct lmp_chan *lc = &chan->lc;

    if (strlen(string) > 9) {
        debug_printf("aos_rpc_send_string currently does not support long strings T_T\n");
    }
    char buf[9];
    memcpy(buf, string, 9);

    errval_t err;
    err = lmp_chan_send(lc, LMP_SEND_FLAGS_DEFAULT, NULL_CAP, 8, buf[0], 
                            buf[1], buf[2], buf[3], buf[4],
                            buf[5], buf[6], buf[7], buf[8]);

    if (err_is_fail(err)) {
        return err;
    }
    
    return SYS_ERR_OK;
}

errval_t aos_rpc_get_ram_cap(struct aos_rpc *chan, size_t request_bits,
                             struct capref *retcap, size_t *ret_bits)
{
    // TODO: implement functionality to request a RAM capability over the
    // given channel and wait until it is delivered.
    errval_t err;
    err = cap_create(*retcap, ObjType_RAM, request_bits);

    if (err_is_fail(err)) {
        return err;
    }  
    
    

    return SYS_ERR_OK;
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

errval_t aos_rpc_init(struct aos_rpc *rpc)
{
    // TODO: Initialize given rpc channel
    errval_t err;

    struct lmp_endpoint *my_ep; 
    err = lmp_endpoint_setup(0, FIRSTEP_BUFLEN, &my_ep);
    
    if (err_is_fail(err)) {
        debug_printf("aos_rpc_init FAIL. err: %d\n", err);
    } else {
        debug_printf("aos_rpc_init, you're ready for RPCs\n");
    }

    waitset_init(get_default_waitset);

    return SYS_ERR_OK;
}
