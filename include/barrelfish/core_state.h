/**
 * \file
 * \brief
 */

/*
 * Copyright (c) 2008, 2009, 2010, 2011, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#ifndef LIBBARRELFISH_CORESTATE_H
#define LIBBARRELFISH_CORESTATE_H

#include <k_r_malloc.h>
#include <barrelfish/paging.h>
#include <barrelfish/waitset.h>

struct morecore_state {
    struct thread_mutex mutex;
    Header header_base;
    Header *header_freep;
    // for "real" morecore (lib/barrelfish/morecore.c)
    struct paging_region region;
    // for "static" morecore (see lib/barrelfish/static_morecore.c)
    char *freep;
};

struct ram_alloc_state {
    bool mem_connect_done;
    errval_t mem_connect_err;
    struct thread_mutex ram_alloc_lock;
    ram_alloc_func_t ram_alloc_func;
    uint64_t default_minbase;
    uint64_t default_maxlimit;
    int base_capnum;
};

struct skb_state {
    bool request_done;
    struct skb_rpc_client *skb;
};

struct slot_alloc_state {
    struct multi_slot_allocator defca;

    struct single_slot_allocator top;
    struct slot_allocator_list head;
    struct slot_allocator_list reserve;

    char     top_buf[SINGLE_SLOT_ALLOC_BUFLEN(SLOT_ALLOC_CNODE_SLOTS)];
    char    head_buf[SINGLE_SLOT_ALLOC_BUFLEN(SLOT_ALLOC_CNODE_SLOTS)];
    char reserve_buf[SINGLE_SLOT_ALLOC_BUFLEN(SLOT_ALLOC_CNODE_SLOTS)];
    char    root_buf[SINGLE_SLOT_ALLOC_BUFLEN(DEFAULT_CNODE_SLOTS   )];

    struct single_slot_allocator rootca;
};

struct terminal_state;
struct octopus_rpc_client;
struct domain_state;
struct spawn_state;
struct monitor_binding;
struct aos_chan;
struct mem_rpc_client;
struct spawn_rpc_client;
struct paging_state;

struct core_state_generic {
    struct waitset default_waitset;
    struct monitor_binding *monitor_binding;
    struct aos_chan *init_chan;
    struct monitor_blocking_rpc_client *monitor_blocking_rpc_client;
    struct mem_rpc_client *mem_st;
    struct morecore_state morecore_state;
    struct paging_state *paging_state;
    struct ram_alloc_state ram_alloc_state;
    struct octopus_rpc_client *nameservice_rpc_client;
    struct spawn_rpc_client *spawn_rpc_clients[MAX_CPUS];
    struct terminal_state *terminal_state;
    struct domain_state *domain_state;
    struct spawn_state *spawn_state;
    struct slot_alloc_state slot_alloc_state;
    struct skb_state skb_state;
};

#endif
