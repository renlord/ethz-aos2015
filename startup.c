/**
 * \file
 * \brief Architecture-independent bootstrap code.
 */

/*
 * Copyright (c) 2007, 2008, 2009, 2010, 2011, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#include <string.h>
#include <kernel.h>
#include <startup.h>
#include <exec.h>
#include <dispatch.h>
#include <barrelfish_kpi/init.h>
#include <barrelfish_kpi/paging_arch.h>
#include <barrelfish_kpi/domain_params.h>

coreid_t my_core_id;

struct dcb *allocate_dcb(struct spawn_state *st,
                         const char *name, int argc, const char** argv,
                         lpaddr_t bootinfo, lvaddr_t args_base,
                         alloc_phys_func alloc_phys, lvaddr_t *retparamaddr)
{
    printf("spawn_module\n");

    // check for reuse of static state
#ifndef NDEBUG
    static bool once_only;
    assert(!once_only);
    once_only = true;
#endif

    // allocate init dcb
    struct dcb *init_dcb = (struct dcb *)alloc_phys(1UL << OBJBITS_DISPATCHER);

    // allocate arguments page
    st->args_page = gen_phys_to_local_phys(alloc_phys(ARGS_SIZE));

    /* Initialize dispatcher */
    dispatcher_handle_t init_handle
        = local_phys_to_mem(alloc_phys(1 << DISPATCHER_FRAME_BITS));
    struct dispatcher_shared_generic *init_disp =
        get_dispatcher_shared_generic(init_handle);
    strncpy(init_disp->name, argv[0], DISP_NAME_LEN);

    /* Set fields in DCB */
    // Set disp
    init_dcb->disp = init_handle;

    // XXX: hack for 1:1 mapping
    if (args_base == 0) {
        args_base = st->args_page;
    }

    /* Construct args page */
    struct spawn_domain_params *params = (void *)local_phys_to_mem(st->args_page);
    memset(params, 0, sizeof(*params));
    char *buf = (char *)local_phys_to_mem(st->args_page
                                          + sizeof(struct spawn_domain_params));
    size_t buflen = ARGS_SIZE - sizeof(struct spawn_domain_params);
    assert(argc < MAX_CMDLINE_ARGS);
    params->argc = argc;
    for (int i = 0; i < argc; i++) {
        size_t arglen = strlen(argv[i]);
        assert(arglen < buflen);
        params->argv[i] = (void *)(args_base + mem_to_local_phys((lvaddr_t)buf)
                                   - st->args_page);
        strcpy(buf, argv[i]);
        buf += arglen + 1;
        buflen -= arglen + 1;
    }

    assert(retparamaddr != NULL);
    *retparamaddr = args_base;

    return init_dcb;
}
