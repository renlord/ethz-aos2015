/**
 * \file
 * \brief Barrelfish paging helpers.
 */

/*
 * Copyright (c) 2012, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */


#ifndef LIBBARRELFISH_PAGING_H
#define LIBBARRELFISH_PAGING_H

#include <errors/errno.h>
#include <barrelfish/capabilities.h>
#include <barrelfish/aos_rpc.h>

typedef int paging_flags_t;

#define VADDR_OFFSET ((lvaddr_t)1UL*1024*1024*1024) // 1GB

#define SLAB_BUFSIZE 16
#define ARM_L1_USER_OFFSET(addr) ((addr)>>22 & 0x3ff)
#define ARM_L1_USER_ENTRIES 1024u
#define ARM_L2_USER_OFFSET(addr) ((addr)>>12 & 0x3ff)
#define ARM_L2_USER_ENTRIES 1024u

#define VREGION_FLAGS_READ     0x01 // Reading allowed
#define VREGION_FLAGS_WRITE    0x02 // Writing allowed
#define VREGION_FLAGS_EXECUTE  0x04 // Execute allowed
#define VREGION_FLAGS_NOCACHE  0x08 // Caching disabled
#define VREGION_FLAGS_MPB      0x10 // Message passing buffer
#define VREGION_FLAGS_GUARD    0x20 // Guard page
#define VREGION_FLAGS_MASK     0x2f // Mask of all individual VREGION_FLAGS

#define VREGION_FLAGS_READ_WRITE \
    (VREGION_FLAGS_READ | VREGION_FLAGS_WRITE)
#define VREGION_FLAGS_READ_EXECUTE \
    (VREGION_FLAGS_READ | VREGION_FLAGS_EXECUTE)
#define VREGION_FLAGS_READ_WRITE_NOCACHE \
    (VREGION_FLAGS_READ | VREGION_FLAGS_WRITE | VREGION_FLAGS_NOCACHE)
#define VREGION_FLAGS_READ_WRITE_MPB \
    (VREGION_FLAGS_READ | VREGION_FLAGS_WRITE | VREGION_FLAGS_MPB)

#define ENTRIES_PER_FRAME 32
#define L1_ENTRIES (ARM_L1_OFFSET(VADDR_OFFSET)>>2)
#define NO_OF_FRAMES \
    (L1_ENTRIES*(ARM_L2_MAX_ENTRIES<<2)/ENTRIES_PER_FRAME)
        
struct node {
    lvaddr_t addr;
    size_t max_size;
    bool allocated;
    struct node *left;
    struct node *right;
};

// struct to store the paging status of a process
struct paging_state {
    struct node *root;
    struct node *next_node;
    struct node all_nodes[ARM_L1_MAX_ENTRIES*10];
    
    struct capref l1_cap;
    struct capref l2_caps[L1_ENTRIES];
    struct capref frame_caps[NO_OF_FRAMES];
    struct capref *next_frame;
    struct capref guard_cap;
    struct aos_rpc *rpc;
};

struct thread;

// Debug function for checking amount of free space in
// buddy allocation
void debug_get_free_space(size_t *free_s, size_t *alloc_s, size_t *max_s);

/// Initialize paging_state struct
errval_t paging_init_state(struct paging_state *st, lvaddr_t start_vaddr,
        struct capref pdir);
        
/// initialize self-paging module
errval_t paging_init(void);

/// setup paging on new thread (used for user-level threads)
void paging_init_onthread(struct thread *t);

struct paging_region {
    lvaddr_t base_addr;
    lvaddr_t current_addr;
    size_t region_size;
};

errval_t paging_region_init(struct paging_state *st,
                            struct paging_region *pr, size_t size);

/**
 * \brief return a pointer to a bit of the paging region `pr`.
 * This function gets used in some of the code that is responsible
 * for allocating Frame (and other) capabilities.
 */
errval_t paging_region_map(struct paging_region *pr, size_t req_size,
                           void **retbuf, size_t *ret_size);
/**
 * \brief free a bit of the paging region `pr`.
 * This function gets used in some of the code that is responsible
 * for allocating Frame (and other) capabilities.
 * We ignore unmap requests right now.
 */
errval_t paging_region_unmap(struct paging_region *pr, lvaddr_t base, size_t bytes);

/**
 * \brief Find a bit of free virtual address space that is large enough to
 *        accomodate a buffer of size `bytes`.
 */
errval_t paging_alloc(struct paging_state *st, void **buf, size_t bytes);

/**
 * \brief TODO
 */
errval_t paging_dealloc(struct paging_state *st, void *buf);

/**
 * Functions to map a user provided frame.
 */
/// Map user provided frame with given flags while allocating VA space for it
errval_t paging_map_frame_attr(struct paging_state *st, void **buf,
                    size_t bytes, struct capref frame,
                    int flags, void *arg1, void *arg2);
                    
/// Map user provided frame at user provided VA with given flags.
errval_t paging_map_fixed_attr(struct paging_state *st, lvaddr_t vaddr,
                struct capref frame, size_t bytes, int flags);

/**
 * \brief unmap a user provided frame
 * NOTE: this function is currently here to make libbarrelfish compile. As
 * noted on paging_region_unmap we ignore unmap requests right now.
 */
errval_t paging_unmap(struct paging_state *st, const void *region);

/**
 * \brief maps device frame to userland virtual memory space 
 */
errval_t paging_map_user_device(struct paging_state *st, lvaddr_t addr, struct capref frame_cap, 
        uint64_t start_offset, size_t length, int flags);

/// Map user provided frame while allocating VA space for it
static inline errval_t paging_map_frame(struct paging_state *st, void **buf,
                size_t bytes, struct capref frame, void *arg1, void *arg2)
{
    return paging_map_frame_attr(st, buf, bytes, frame,
            VREGION_FLAGS_READ_WRITE, arg1, arg2);
}

/// Map user provided frame at user provided VA.
static inline errval_t paging_map_fixed(struct paging_state *st, lvaddr_t vaddr,
                struct capref frame, size_t bytes)
{
    return paging_map_fixed_attr(st, vaddr, frame, bytes,
            VREGION_FLAGS_READ_WRITE);
}

static inline lvaddr_t paging_genvaddr_to_lvaddr(genvaddr_t genvaddr) {
    return (lvaddr_t) genvaddr;
}

#endif // LIBBARRELFISH_PAGING_H
