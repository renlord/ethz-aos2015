/**
 * \file
 * \brief Barrelfish paging helpers.
 */

/*
 * Copyright (c) 2012, 2013, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#include <barrelfish/barrelfish.h>
#include <barrelfish/paging.h>
#include <barrelfish/except.h>
#include <barrelfish/slab.h>
#include "threads_priv.h"

#include <stdio.h>
#include <string.h>

// Copied from lib/barrelfish/threads.c
#define EXC_STACK_ALIGNMENT (sizeof(uint64_t) * 2)

// Our exception stack size
#define EXC_STACK_SIZE 256*EXC_STACK_ALIGNMENT

#define V_OFFSET 1UL << 26

#define THROW_ERROR *((char*)0) = 'x'

#define MAX(a,b) \
    ({ __typeof__ (a) _a = (a); \
     __typeof__ (b) _b = (b); \
     _a > _b ? _a : _b; })
         
#define MIN(a,b) \
    ({ __typeof__ (a) _a = (a); \
     __typeof__ (b) _b = (b); \
     _a < _b ? _a : _b; })
         
static struct paging_state current;

/**
 * \brief Helper function that allocates a slot and
 *        creates a ARM l2 page table capability
 */
__attribute__((unused))
static errval_t arml2_alloc(struct capref *ret)
{
    errval_t err;
    err = slot_alloc(ret);
    if (err_is_fail(err)) {
        debug_printf("slot_alloc failed: %s\n", err_getstring(err));
        return err;
    }
    err = vnode_create(*ret, ObjType_VNode_ARM_l2);
    if (err_is_fail(err)) {
        debug_printf("vnode_create failed: %s\n", err_getstring(err));
        return err;
    }
    return SYS_ERR_OK;
}


errval_t avl_insert_node(struct paging_state *st, void **buf, size_t bytes);

errval_t avl_insert_node(struct paging_state *st, void **buf, size_t bytes)
{
    if(!st)
        return SYS_ERR_OK;

    lvaddr_t addr = st->addrs[st->next_free];
    *((lvaddr_t*)buf) = addr;

    st->sizes[st->next_free] = bytes;
    st->addrs[++st->next_free] = ROUND_UP(addr+bytes, BASE_PAGE_SIZE);

    debug_printf("in avl_insert_node for st 0x%08x\n", st);
    debug_printf("addrs changed from 0x%08x to 0x%08x\n",
                 addr, st->addrs[st->next_free]);
    debug_printf("next_free incremented from %d to %d\n",
                 st->next_free-1, st->next_free);

    return SYS_ERR_OK;
}


uint32_t stack[EXC_STACK_SIZE];

void page_fault_handler(enum exception_type type, int subtype,
                        void *addr, arch_registers_state_t *regs,
                        arch_registers_fpu_state_t *fpuregs);

// TODO: implement page fault handler that installs frames when a page fault
// occurs and keeps track of the virtual address space.
void page_fault_handler(enum exception_type type, int subtype,
                        void *addr, arch_registers_state_t *regs,
                        arch_registers_fpu_state_t *fpuregs)
{
    if (type != EXCEPT_PAGEFAULT){
        // TODO handle other pagefaults
        return;
    }
    errval_t err;
    
    // Cast for easier calculations
    lvaddr_t vaddr = (lvaddr_t)addr;

    
    // Primitive way of finding next address
    size_t bytes = 0;
    for (uint32_t i = 0; i < current.next_free; i++){
        if(current.addrs[i] <= vaddr &&
           current.addrs[i] + current.sizes[i] > vaddr){
               bytes = current.sizes[i];
        }
    }
    
    if (bytes == 0) {
        debug_printf("Did not find address 0x%08x!!\n", vaddr);
        THROW_ERROR;
    }
    
    
    // Relevant pagetable slots
    cslot_t l1_slot = ARM_L1_OFFSET(vaddr)>>2;
    cslot_t l2_slot = ARM_L2_OFFSET(vaddr)+(ARM_L1_OFFSET(vaddr)%4)*ARM_L2_MAX_ENTRIES;

    // Check if we have l2 capabilities and request if not
    struct capref *l2_cap = &current.l2_caps[l1_slot];
    if(capref_is_null(*l2_cap)) {
        // Allocate capability
        arml2_alloc(l2_cap);
    
        // Insert L2 pagetable in L1 pagetable
        err = vnode_map(current.l1_cap, *l2_cap, l1_slot,
                        VREGION_FLAGS_READ_WRITE, 0, 1);
    
        if (err != SYS_ERR_OK){
            debug_printf("Could not insert L2 pagetable in L1 pagetable for addr 0x%08x: %s\n",
                         vaddr, err_getstring(err));
            THROW_ERROR;
        }
    }
    
    // Allocate frame capabilities
    uint32_t frame_idx = (l1_slot*ARM_L1_MAX_ENTRIES*4+l2_slot)/ENTRIES_PER_FRAME;
    struct capref *frame_cap = &current.frame_caps[frame_idx];
    
    // If we already have a capability stored for this frame, something went wrong
    if (!capref_is_null(*frame_cap)) {
        debug_printf("Page fault occured for already mapped frame.\n");
        THROW_ERROR;
    }
    
    // We don't have capability so request
    size_t retsize;
    frame_alloc(frame_cap, BASE_PAGE_SIZE*ENTRIES_PER_FRAME, &retsize);
    if (retsize != BASE_PAGE_SIZE*ENTRIES_PER_FRAME){
        debug_printf("Tried to allocate %d bytes but could only allocate %d.\n",
                     BASE_PAGE_SIZE*ENTRIES_PER_FRAME, retsize);
        THROW_ERROR;
    }
    
    // Map frame capability in L2 pagetable
    err = vnode_map(*l2_cap, *frame_cap, ROUND_DOWN(l2_slot, ENTRIES_PER_FRAME),
                    VREGION_FLAGS_READ_WRITE, 0, ENTRIES_PER_FRAME);
        if (err != SYS_ERR_OK){
        debug_printf("Could not insert frame in L2 pagetable for addr 0x%08x: %s\n",
                     vaddr, err_getstring(err));
        THROW_ERROR;                
    }
    
}



errval_t paging_init_state(struct paging_state *st, lvaddr_t start_vaddr,
        struct capref pdir)
{
    debug_printf("paging_init_state\n");
    
    start_vaddr = MAX(start_vaddr, V_OFFSET);
    
    // TODO: implement state struct initialization
    st->l1_cap = pdir;
    current = *st;
    struct avl_node root_v2p = {{start_vaddr, 0, 0}};
    
    st->root_v2p = root_v2p;
    st->addrs[0] = start_vaddr;
    st->next_free = 0;
    
    for(uint32_t i = 0; i < ARM_L1_MAX_ENTRIES; i++){
        st->l2_caps[i] = NULL_CAP;
    }
    
    for(uint32_t i = 0; i < ARM_L1_MAX_ENTRIES*ARM_L2_MAX_ENTRIES/ENTRIES_PER_FRAME; i++){
        // st->frame_caps[i] = NULL_CAP;
    }
    
    return SYS_ERR_OK;
}


errval_t paging_init(void)
{
    debug_printf("paging_init\n");
    // TODO: initialize self-paging handler
    
    
    // TIP: use thread_set_exception_handler() to setup a page fault handler
    // TIP: Think about the fact that later on, you'll have to make sure that
    // you can handle page faults in any thread of a domain.
    
    void *old_stack_base, *old_stack_top;
    thread_set_exception_handler(page_fault_handler, NULL,
                                 stack, &stack[EXC_STACK_SIZE],
                                 &old_stack_base, &old_stack_top);

    printf("From paging_init: 0x%08x\n", page_fault_handler);
    printf("Old stack base: 0x%08x, old stack top: 0x%08x\n", old_stack_base, old_stack_top);
    // TIP: it might be a good idea to call paging_init_state() from here to
    // avoid code duplication.
    struct capref l1_cap = {
        .cnode = cnode_page,
        .slot = 0,
    };

    printf("l1_cap addr: 0x%08x\n", get_cap_addr(l1_cap));

    paging_init_state(&current, 0, l1_cap);
    
    set_current_paging_state(&current);
    return SYS_ERR_OK;
}

void paging_init_onthread(struct thread *t)
{
    // TODO: setup exception handler for thread `t'.
    t->exception_handler = page_fault_handler;
}

/**
 * \brief return a pointer to a bit of the paging region `pr`.
 * This function gets used in some of the code that is responsible
 * for allocating Frame (and other) capabilities.
 */
errval_t paging_region_init(struct paging_state *st, struct paging_region *pr, size_t size)
{
    void *base;
    errval_t err = paging_alloc(st, &base, size);
    if (err_is_fail(err)) {
        debug_printf("paging_region_init: paging_alloc failed\n");
        return err_push(err, LIB_ERR_VSPACE_MMU_AWARE_INIT);
    }
    pr->base_addr    = (lvaddr_t)base;
    pr->current_addr = pr->base_addr;
    pr->region_size  = size;
    // TODO: maybe add paging regions to paging state?
    return SYS_ERR_OK;
}

/**
 * \brief return a pointer to a bit of the paging region `pr`.
 * This function gets used in some of the code that is responsible
 * for allocating Frame (and other) capabilities.
 */
errval_t paging_region_map(struct paging_region *pr, size_t req_size,
                           void **retbuf, size_t *ret_size)
{
    lvaddr_t end_addr = pr->base_addr + pr->region_size;
    ssize_t rem = end_addr - pr->current_addr;
    if (rem > req_size) {
        // ok
        *retbuf = (void*)pr->current_addr;
        *ret_size = req_size;
        pr->current_addr += req_size;
    } else if (rem > 0) {
        *retbuf = (void*)pr->current_addr;
        *ret_size = rem;
        pr->current_addr += rem;
        debug_printf("exhausted paging region, "
                "expect badness on next allocation\n");
    } else {
        return LIB_ERR_VSPACE_MMU_AWARE_NO_SPACE;
    }
    return SYS_ERR_OK;
}

/**
 * \brief free a bit of the paging region `pr`.
 * This function gets used in some of the code that is responsible
 * for allocating Frame (and other) capabilities.
 * We ignore unmap requests right now.
 */
errval_t paging_region_unmap(struct paging_region *pr, lvaddr_t base, size_t bytes)
{
    // XXX: should free up some space in paging region, however need to track
    //      holes for non-trivial case
    return SYS_ERR_OK;
}

/**
 * \brief Find a bit of free virtual address space that is large enough to
 *        accomodate a buffer of size `bytes`.
 * TODO: you need to implement this function using the knowledge of your
 * self-paging implementation about where you have already mapped frames.
 */
errval_t paging_alloc(struct paging_state *st, void **buf, size_t bytes)
{
    debug_printf("paging_alloc called\n");
    if(!st){
        debug_printf("arg st is NULL");
        return SYS_ERR_OK;
    }
        
    current = *st;

    // find virtual address from AVL-tree
    errval_t err = avl_insert_node(st, buf, bytes);
    if (err_is_fail(err)){
        return err;
    }
                    
    return SYS_ERR_OK;
}

/**
 * \brief map a user provided frame, and return the VA of the mapped
 *        frame in `buf`.
 */
errval_t paging_map_frame_attr(struct paging_state *st, void **buf,
                               size_t bytes, struct capref frame,
                               int flags, void *arg1, void *arg2)
{
    errval_t err = paging_alloc(st, buf, bytes);
    if (err_is_fail(err)) {
        return err;
    }


    return paging_map_fixed_attr(st, (lvaddr_t)(*buf), frame, bytes, flags);
}


/**
 * \brief map a user provided frame at user provided VA.
 */
errval_t paging_map_fixed_attr(struct paging_state *st, lvaddr_t vaddr,
        struct capref frame, size_t bytes, int flags)
{
    // TODO: you will need this functionality in later assignments. Try to
    // keep this in mind when designing your self-paging system.
    return SYS_ERR_OK;
}

/**
 * \brief unmap a user provided frame, and return the VA of the mapped
 *        frame in `buf`.
 * NOTE: this function is currently here to make libbarrelfish compile. As
 * noted on paging_region_unmap we ignore unmap requests right now.
 */
errval_t paging_unmap(struct paging_state *st, const void *region)
{
    return SYS_ERR_OK;
}
