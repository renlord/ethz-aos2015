/**
 * \file
 * \brief Barrelfish library initialization.
 */

/*
 * Copyright (c) 2007, 2008, 2009, 2010, 2011, 2012, 2013 ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#include <stdio.h>
#include <barrelfish/barrelfish.h>
#include <barrelfish/idc.h>
#include <barrelfish/dispatch.h>
#include <barrelfish/curdispatcher_arch.h>
#include <barrelfish/dispatcher_arch.h>
#include <barrelfish_kpi/dispatcher_shared.h>
#include <barrelfish/morecore.h>
#include <barrelfish/monitor_client.h>
#include <barrelfish/nameservice_client.h>
#include <barrelfish/paging.h>
#include <barrelfish/aos_rpc.h>
#include <barrelfish_kpi/domain_params.h>
#include <if/monitor_defs.h>
#include <trace/trace.h>
#include <octopus/init.h>
#include "threads_priv.h"
#include "init.h"

/// Are we the init domain (and thus need to take some special paths)?
static bool init_domain;

extern size_t (*_libc_terminal_read_func)(char *, size_t);
extern size_t (*_libc_terminal_write_func)(const char *, size_t);
extern void (*_libc_exit_func)(int);
extern void (*_libc_assert_func)(const char *, const char *, const char *, int);

void libc_exit(int);

void libc_exit(int status)
{
    // Use spawnd if spawned through spawnd
    if(disp_get_domain_id() == 0) {
        errval_t err = cap_revoke(cap_dispatcher);
        if (err_is_fail(err)) {
            sys_print("revoking dispatcher failed in _Exit, spinning!", 100);
            while (1) {}
        }
        err = cap_delete(cap_dispatcher);
        sys_print("deleting dispatcher failed in _Exit, spinning!", 100);

        // XXX: Leak all other domain allocations
    } else {
        // we will call spawnd service to clean a domain
        aos_rpc_send_string(&local_rpc, "bye");
        thread_exit();
        //event_dispatch(get_default_waitset());
        // errval_t err = cap_revoke(cap_dispatcher);
        // if (err_is_fail(err)) {
        //     sys_print("revoking dispatcher failed in _Exit, spinning!", 100);
        //     while (1) {}
        // }
        // err = cap_delete(cap_dispatcher);
        // sys_print("deleting dispatcher failed in _Exit, spinning!", 100);
        // debug_printf("libc_exit NYI!\n");
    }

    // If we're not dead by now, we wait
    while (1) {}
}

static void libc_assert(const char *expression, const char *file,
                        const char *function, int line)
{
    char buf[512];
    size_t len;

    /* Formatting as per suggestion in C99 spec 7.2.1.1 */
    len = snprintf(buf, sizeof(buf), "Assertion failed on core %d in %.*s: %s,"
                   " function %s, file %s, line %d.\n",
                   disp_get_core_id(), DISP_NAME_LEN,
                   disp_name(), expression, function, file, line);
    sys_print(buf, len < sizeof(buf) ? len : sizeof(buf));
}

static size_t syscall_terminal_write(const char *buf, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            aos_rpc_serial_putchar(&local_rpc, '\r');
        }
        errval_t err = aos_rpc_serial_putchar(&local_rpc, buf[i]);
        if (err_is_fail(err)) {
            debug_printf("userland printf fail! %s\n", err_getstring(err));
            err_print_calltrace(err);
            abort();
            //break;
        }   
    }
    //debug_printf("characters printed: %d\n", n);
    return i;
}

// static void scan_line(char *buf, size_t len) 
// {   
//     for (int i = 0; i < len; i++, buf++) {
//         errval_t err = aos_rpc_serial_getchar(&local_rpc, buf);
//         if (err_is_fail(err)) {
//             debug_printf("userland scan_line fail! %s\n", err_getstring(err));
//             err_print_calltrace(err);
//             break;
//         }
//         buf++;
//     }
// }


static size_t dummy_terminal_read(char *buf, size_t len)
{
    errval_t err = aos_rpc_serial_getchar(&local_rpc, buf);
    if (err_is_fail(err)) {
        debug_printf("userland scan_line fail! %s\n", err_getstring(err));
        err_print_calltrace(err);
        //break;
    }
    return len;
}

/* Set libc function pointers */
void barrelfish_libc_glue_init(void)
{
    // TODO: change these to use the user-space serial driver if possible
    _libc_terminal_read_func = dummy_terminal_read;
    _libc_terminal_write_func = syscall_terminal_write;
    _libc_exit_func = libc_exit;
    _libc_assert_func = libc_assert;
    /* morecore func is setup by morecore_init() */

    // XXX: set a static buffer for stdout
    // this avoids an implicit call to malloc() on the first printf
    static char buf[BUFSIZ];
    setvbuf(stdout, buf, _IOLBF, sizeof(buf));
    static char ebuf[BUFSIZ];
    setvbuf(stderr, ebuf, _IOLBF, sizeof(buf));
}

#if 0
static bool register_w_init_done = false;
static void init_recv_handler(struct aos_chan *ac, struct lmp_recv_msg *msg, struct capref cap)
{
    assert(ac == get_init_chan());
    debug_printf("in libbf init_recv_handler\n");
    register_w_init_done = true;
}
#endif

/** \brief Initialise libbarrelfish.
 *
 * This runs on a thread in every domain, after the dispatcher is setup but
 * before main() runs.
 */
errval_t barrelfish_init_onthread(struct spawn_domain_params *params)
{
    errval_t err;

    // do we have an environment?
    if (params != NULL && params->envp[0] != NULL) {
        extern char **environ;
        environ = params->envp;
    }

    // Init default waitset for this dispatcher
    struct waitset *default_ws = get_default_waitset();
    waitset_init(default_ws);

    // Initialize ram_alloc state
    ram_alloc_init();
    /* All domains use smallcn to initialize */
    err = ram_alloc_set(ram_alloc_fixed);
    if (err_is_fail(err)) {
        return err_push(err, LIB_ERR_RAM_ALLOC_SET);
    }

    err = paging_init();
    if (err_is_fail(err)) {
        return err_push(err, LIB_ERR_VSPACE_INIT);
    }

    err = slot_alloc_init();
    if (err_is_fail(err)) {
        return err_push(err, LIB_ERR_SLOT_ALLOC_INIT);
    }

    err = morecore_init();
    if (err_is_fail(err)) {
        return err_push(err, LIB_ERR_MORECORE_INIT);
    }

    lmp_endpoint_init();

    // init domains only get partial init
    if (init_domain) {
        return SYS_ERR_OK;
    }
    
    err = aos_rpc_init(&local_rpc);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "failed to initialize aos_rpc for process\n");
        err_print_calltrace(err);
    }
    
    // right now we don't have the nameservice & don't need the terminal
    // and domain spanning, so we return here
    return SYS_ERR_OK;
}


/**
 *  \brief Initialise libbarrelfish, while disabled.
 *
 * This runs on the dispatcher's stack, while disabled, before the dispatcher is
 * setup. We can't call anything that needs to be enabled (ie. cap invocations)
 * or uses threads. This is called from crt0.
 */
void barrelfish_init_disabled(dispatcher_handle_t handle, bool init_dom_arg);
void barrelfish_init_disabled(dispatcher_handle_t handle, bool init_dom_arg)
{
    init_domain = init_dom_arg;
    disp_init_disabled(handle);
    thread_init_disabled(handle, init_dom_arg);
}
