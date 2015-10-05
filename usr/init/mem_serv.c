/**
 * \file
 * \brief
 */

/*
 * Copyright (c) 2012, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#include <string.h>
#include "init.h"
#include <mm/mm.h>

size_t mem_total = 0, mem_avail = 0;

#define MAXSIZEBITS     31                 ///< Max size of each allocation
                                           ///  must be less than #bits of arch.
#define MINSIZEBITS     OBJBITS_DISPATCHER ///< Min size of each allocation
#define MAXCHILDBITS    4                  ///< Max branching of BTree nodes
/// Maximum depth of the BTree, assuming only branching by two at each level
#define MAXDEPTH        (MAXSIZEBITS - MINSIZEBITS + 1)
/// Maximum number of BTree nodes
#define NNODES          ((1UL << MAXDEPTH) - 1)

// size of cnodes created by slot allocator
#define CNODE_BITS      12
#define NCNODES         (1UL << CNODE_BITS)     ///< Maximum number of CNodes

/// Watermark at which we must refill the slab allocator used for nodes
#define MINSPARENODES   (MAXDEPTH * 9) // XXX: FIXME: experimentally determined!

/// General-purpose slot allocator
static struct multi_slot_allocator msa;
/// MM allocator instance data
static struct mm mm_ram;
/// Slot allocator for MM
static struct slot_prealloc ram_slot_alloc;

static bool refilling = false;

static errval_t memserv_alloc(struct capref *ret, uint8_t bits, genpaddr_t minbase,
                              genpaddr_t maxlimit)
{
    errval_t err;

    assert(bits >= MINSIZEBITS);

    /* refill slot allocator if needed */
    err = slot_prealloc_refill(mm_ram.slot_alloc_inst);
    assert(err_is_ok(err));

    /* refill slab allocator if needed */
    size_t freecount = slab_freecount(&mm_ram.slabs);
    while (!refilling && (freecount <= MINSPARENODES)) {
        refilling = true;
        struct capref frame;
        err = msa.a.alloc(&msa.a, &frame);
        assert(err_is_ok(err));
        size_t retsize;
        err = frame_create(frame, BASE_PAGE_SIZE * 8, &retsize);
        assert(err_is_ok(err));
        assert(retsize % BASE_PAGE_SIZE == 0);
        assert(retsize >= BASE_PAGE_SIZE);
        void *buf;
        err = paging_map_frame(get_current_paging_state(), &buf, retsize, frame, NULL, NULL);
        if (err_is_fail(err)) {
            DEBUG_ERR(err, "paging_map_frame failed");
            assert(buf);
        }
        slab_grow(&mm_ram.slabs, buf, retsize);
        freecount = slab_freecount(&mm_ram.slabs);
    }
    if (freecount > MINSPARENODES) {
        refilling = false;
    }

    if(maxlimit == 0) {
        err = mm_alloc(&mm_ram, bits, ret, NULL);
    } else {
        err = mm_alloc_range(&mm_ram, bits, minbase, maxlimit, ret, NULL);
    }

    if (err_is_fail(err)) {
        debug_printf("in mem_serv:mymm_alloc(bits=%"PRIu8", minbase=%"PRIxGENPADDR
                     ", maxlimit=%"PRIxGENPADDR")\n", bits, minbase, maxlimit);
        DEBUG_ERR(err, "mem_serv:mymm_alloc");
    }

    return err;
}

errval_t initialize_mem_serv(void)
{
    errval_t err;

    /* Step 1: Initialize slot allocator by passing a cnode cap for it to start with */
    struct capref cnode_cap;
    err = slot_alloc(&cnode_cap);
    assert(err_is_ok(err));
    struct capref cnode_start_cap = { .slot  = 0 };

    struct capref ram;
    err = ram_alloc_fixed(&ram, BASE_PAGE_BITS, 0, 0);
    assert(err_is_ok(err));
    err = cnode_create_from_mem(cnode_cap, ram, &cnode_start_cap.cnode,
                              DEFAULT_CNODE_BITS);
    assert(err_is_ok(err));

    /* location where slot allocator will place its top-level cnode */
    struct capref top_slot_cap = {
        .cnode = cnode_root,
        .slot = ROOTCN_SLOT_SLOT_ALLOCR,
    };

    /* clear mm_ram struct */
    memset(&mm_ram, 0, sizeof(mm_ram));

    /* init slot allocator */
    err = slot_prealloc_init(&ram_slot_alloc, top_slot_cap, MAXCHILDBITS,
                           CNODE_BITS, cnode_start_cap,
                           1UL << DEFAULT_CNODE_BITS, &mm_ram);
    assert(err_is_ok(err));

    // FIXME: remove magic constant for lowest valid RAM address
    err = mm_init(&mm_ram, ObjType_RAM, 0x80000000,
                MAXSIZEBITS, MAXCHILDBITS, NULL,
                slot_alloc_prealloc, &ram_slot_alloc, true);
    assert(err_is_ok(err));

    /* Step 2: give MM allocator static storage to get it started */
    static char nodebuf[SLAB_STATIC_SIZE(MINSPARENODES, MM_NODE_SIZE(MAXCHILDBITS))];
    slab_grow(&mm_ram.slabs, nodebuf, sizeof(nodebuf));

    /* Step 3: walk bootinfo and add all unused RAM caps to allocator */
    struct capref mem_cap = {
        .cnode = cnode_super,
        .slot = 0,
    };

    for (int i = 0; i < bi->regions_length; i++) {
        if (bi->regions[i].mr_type == RegionType_Empty) {
            //dump_ram_region(i, bi->regions + i);

            mem_total += ((size_t)1) << bi->regions[i].mr_bits;

            if (bi->regions[i].mr_consumed) {
                // region consumed by init, skipped
                mem_cap.slot++;
                continue;
            }

            err = mm_add(&mm_ram, mem_cap, bi->regions[i].mr_bits,
                         bi->regions[i].mr_base);
            if (err_is_ok(err)) {
                mem_avail += ((size_t)1) << bi->regions[i].mr_bits;
            } else {
                DEBUG_ERR(err, "Warning: adding RAM region %d (%p/%d) FAILED",
                          i, bi->regions[i].mr_base, bi->regions[i].mr_bits);
            }

            /* try to refill slot allocator (may fail if the mem allocator is empty) */
            err = slot_prealloc_refill(mm_ram.slot_alloc_inst);
            if (err_is_fail(err) && err_no(err) != MM_ERR_SLOT_MM_ALLOC) {
                DEBUG_ERR(err, "in slot_prealloc_refill() while initialising"
                               " memory allocator");
                abort();
            }

            /* refill slab allocator if needed and possible */
            if (slab_freecount(&mm_ram.slabs) <= MINSPARENODES
                && mem_avail > (1UL << (CNODE_BITS + OBJBITS_CTE)) * 2
                                + 10 * BASE_PAGE_SIZE) {
                slab_default_refill(&mm_ram.slabs); // may fail
            }
            mem_cap.slot++;
        }
    }

    err = slot_prealloc_refill(mm_ram.slot_alloc_inst);
    if (err_is_fail(err)) {
        debug_printf("Fatal internal error in RAM allocator: failed to initialise "
               "slot allocator\n");
        DEBUG_ERR(err, "failed to init slot allocator");
        abort();
    }

    debug_printf("RAM allocator initialised, %zd MB (of %zd MB) available\n",
           mem_avail / 1024 / 1024, mem_total / 1024 / 1024);


    // setup proper multi slot alloc
    err = multi_slot_alloc_init(&msa, DEFAULT_CNODE_SLOTS, NULL);
    if(err_is_fail(err)) {
        USER_PANIC_ERR(err, "multi_slot_alloc_init");
    }
    debug_printf("MSA initialised\n");

    // switch over ram alloc to proper ram allocator
    ram_alloc_set(memserv_alloc);

    return SYS_ERR_OK;
}
