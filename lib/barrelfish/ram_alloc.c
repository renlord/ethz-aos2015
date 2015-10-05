/**
 * \file
 * \brief RAM allocator code (client-side)
 */

/*
 * Copyright (c) 2007, 2008, 2009, 2010, 2011, 2012, 2013 ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#include <barrelfish/barrelfish.h>
#include <barrelfish/core_state.h>

#include <if/monitor_defs.h>
#include <if/mem_rpcclient_defs.h>
#include <stdio.h>
#if 0
static bool aos_got_ram_reply = false;
static void aos_ram_alloc_recv(struct aos_chan *ac,struct lmp_recv_msg *msg, struct capref cap)
{
    if (!capref_is_null(cap) && msg->words[0] == AOS_MSGTYPE_RAMALLOC_REPLY) {
        errval_t err = msg->words[1];
        if (err_is_ok(err)) {
            aos_got_ram_reply = true;
        } else {
            USER_PANIC_ERR(err, "ram_alloc reply\n");
        }
    }
}
#endif

/* remote (indirect through a channel) version of ram_alloc, for most domains */
static errval_t ram_alloc_remote(struct capref *ret, uint8_t size_bits,
                                 uint64_t minbase, uint64_t maxlimit)
{
// TODO: cleanup
#if 0
    struct ram_alloc_state *ram_alloc_state = get_ram_alloc_state();
    errval_t err;

    // XXX: the transport that ram_alloc uses will allocate slots,
    // which may cause slot_allocator to grow itself.
    // To grow itself, the slot_allocator needs to call ram_alloc.
    // However, ram_alloc has a mutex to protect the lower level transport code.
    // Therefore, we detect the situation when the slot_allocator
    // may grow itself and grow it before acquiring the lock.
    // Once this code become reentrant, this hack can be removed. -Akhi
    struct slot_alloc_state *sas = get_slot_alloc_state();
    struct slot_allocator *ca = (struct slot_allocator*)(&sas->defca);
    if (ca->space == 1) {
        // slot_alloc() might need to allocate memory: reset memory affinity to
        // the default value
        ram_set_affinity(0, 0);
        do {
                struct capref cap;
                err = slot_alloc(&cap);
                if (err_is_fail(err)) {
                    err = err_push(err, LIB_ERR_SLOT_ALLOC);
                    break;
                }
                err = slot_free(cap);
                if (err_is_fail(err)) {
                    err = err_push(err, LIB_ERR_SLOT_FREE);
                    break;
                }
        } while (0);
        ram_set_affinity(minbase, maxlimit);
        if (err_is_fail(err)) {
            return err;
        }
    }

    assert(ret != NULL);

    thread_mutex_lock(&ram_alloc_state->ram_alloc_lock);
    aos_got_ram_reply = false;

    struct aos_chan *ic = get_init_chan();

    // allocate slot and set as receive slot
    err = slot_alloc(ret);
    if (err_is_fail(err)) {
        return err;
    }
    lmp_chan_set_recv_slot(ic->lc, *ret);

    // set custom handler func & remember old one
    aos_recv_handler_fn old_recv;
    aos_chan_set_recv_handler(ic,aos_ram_alloc_recv,&old_recv);
    uintptr_t size[1];
    size[0] = size_bits;
    err = aos_chan_send_message(ic,AOS_MSGTYPE_RAMALLOC_REQUEST,size,NULL_CAP);
    if (err_is_fail(err)) {
        thread_mutex_unlock(&ram_alloc_state->ram_alloc_lock);
        return err;
    }
    while(!aos_got_ram_reply) {
        messages_wait_and_handle_next();
    }
    // reset recv handler
    aos_chan_set_recv_handler(ic,old_recv,NULL);

    // cap should now be in ret

    thread_mutex_unlock(&ram_alloc_state->ram_alloc_lock);

#endif
    return SYS_ERR_OK;
}

void ram_set_affinity(uint64_t minbase, uint64_t maxlimit)
{
    struct ram_alloc_state *ram_alloc_state = get_ram_alloc_state();
    ram_alloc_state->default_minbase = minbase;
    ram_alloc_state->default_maxlimit = maxlimit;
}

void ram_get_affinity(uint64_t *minbase, uint64_t *maxlimit)
{
    struct ram_alloc_state *ram_alloc_state = get_ram_alloc_state();
    *minbase  = ram_alloc_state->default_minbase;
    *maxlimit = ram_alloc_state->default_maxlimit;
}

#define OBJSPERPAGE_CTE         (1 << (BASE_PAGE_BITS - OBJBITS_CTE))

errval_t ram_alloc_fixed(struct capref *ret, uint8_t size_bits,
                         uint64_t minbase, uint64_t maxlimit)
{
    struct ram_alloc_state *state = get_ram_alloc_state();

    if (size_bits == BASE_PAGE_BITS) {
        // XXX: Return error if check to see if out of slots
        assert(state->base_capnum < OBJSPERPAGE_CTE);
        ret->cnode = cnode_base;
        ret->slot  = state->base_capnum++;
        return SYS_ERR_OK;
    } else {
        return LIB_ERR_RAM_ALLOC_WRONG_SIZE;
    }
}

#include <stdio.h>
#include <string.h>

/**
 * \brief Allocates memory in the form of a RAM capability
 *
 * \param ret Pointer to capref struct, filled-in with allocated cap location
 * \param size_bits Amount of RAM to allocate, as a power of two
 *              slot used for the cap in #ret, if any
 */
errval_t ram_alloc(struct capref *ret, uint8_t size_bits)
{
    struct ram_alloc_state *ram_alloc_state = get_ram_alloc_state();
    assert(ram_alloc_state->ram_alloc_func != NULL);
    errval_t err = ram_alloc_state->
        ram_alloc_func(ret, size_bits, ram_alloc_state->default_minbase,
                       ram_alloc_state->default_maxlimit);
    if(err_is_fail(err)) {
      /*
      DEBUG_ERR(err, "ram_alloc");
      printf("callstack: %p %p %p %p\n",
	     __builtin_return_address(0),
	     __builtin_return_address(1),
	     __builtin_return_address(2),
	     __builtin_return_address(3));
    */

    }
    return err;
}

errval_t ram_available(genpaddr_t *available, genpaddr_t *total)
{
    errval_t err;

    struct mem_rpc_client *mc = get_mem_client();

    err = mc->vtbl.available(mc, available, total);
    if(err_is_fail(err)) {
        USER_PANIC_ERR(err, "available");
    }

    return SYS_ERR_OK;
}


/**
 * \brief Initialize the dispatcher specific state of ram_alloc
 */
void ram_alloc_init(void)
{
    /* Initialize the ram_alloc_state */
    struct ram_alloc_state *ram_alloc_state = get_ram_alloc_state();
    ram_alloc_state->mem_connect_done = false;
    ram_alloc_state->mem_connect_err  = 0;
    thread_mutex_init(&ram_alloc_state->ram_alloc_lock);
    ram_alloc_state->ram_alloc_func   = NULL;
    ram_alloc_state->default_minbase  = 0;
    ram_alloc_state->default_maxlimit = 0;
    ram_alloc_state->base_capnum      = 0;
}

/**
 * \brief Set ram_alloc to the default ram_alloc_remote or to a given function
 *
 * If local_allocator is NULL, it will be initialized to the default
 * remote allocator.
 */
errval_t ram_alloc_set(ram_alloc_func_t local_allocator)
{
    struct ram_alloc_state *ram_alloc_state = get_ram_alloc_state();

    /* Special case */
    if (local_allocator != NULL) {
        ram_alloc_state->ram_alloc_func = local_allocator;
        return SYS_ERR_OK;
    }

    struct aos_chan *ic = get_init_chan();
    assert(ic);
    ram_alloc_state->ram_alloc_func = ram_alloc_remote;
    return SYS_ERR_OK;
}
