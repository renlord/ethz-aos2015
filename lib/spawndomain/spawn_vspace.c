/**
 * \file
 * \brief Code for managing VSpace of a new domain when it is spawned
 */

/*
 * Copyright (c) 2007, 2008, 2009, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#include <barrelfish/barrelfish.h>
#include <spawndomain/spawndomain.h>
#include "spawn.h"

/**
 * \brief Initialize the vspace for the domain being spawned
 *
 * \param vnode The root pagetable cap for the new domain
 */
errval_t spawn_paging_init(struct spawninfo *si, struct capref vnode)
{
    errval_t err;
    si->vspace = calloc(1, sizeof(struct paging_state));
    assert(si->vspace);

    err = paging_init_state(si->vspace, 32*1024UL*1024UL, vnode);
    if (err_is_fail(err)) {
        goto cleanup;
    }

    return SYS_ERR_OK;

cleanup:
    free(si->vspace);
    return err;
}

/**
 * \brief Map one frame anywhere
 */
errval_t spawn_vspace_map_one_frame(struct spawninfo *si, genvaddr_t *retaddr,
                                    struct capref frame, size_t size)
{
    errval_t err;

    void *vbuf;
    err = paging_map_frame(si->vspace, &vbuf, size, frame, NULL, NULL);
    if (err_is_fail(err)) {
        return err;
    }

    *retaddr = (genvaddr_t)(lvaddr_t) vbuf;
    return SYS_ERR_OK;

}

/**
 * \brief Map one frame at the given addr
 */
errval_t spawn_vspace_map_fixed_one_frame(struct spawninfo *si, genvaddr_t addr,
                                          struct capref frame, size_t size)
{
    return paging_map_fixed(si->vspace, (lvaddr_t)addr, frame, size);
}
