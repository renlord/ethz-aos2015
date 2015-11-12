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
#include <barrelfish/dispatch.h>
#include "threads_priv.h"

#include <stdio.h>
#include <string.h>

// Copied from lib/barrelfish/threads.c
#define EXC_STACK_ALIGNMENT (sizeof(uint64_t) * 2)

// Our exception stack size
#define EXC_STACK_SIZE (256*EXC_STACK_ALIGNMENT)
#define V_OFFSET (1UL << 25)
#define ALLOCED_BLOB(n) (n->allocated && !n->left && !n->right)
#define MAX_STRUCT_SIZE (1UL << 10)
#define MIN_BLOB_SIZE 4 // minimum size to allocate

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

size_t get_max(struct node *x, struct node *y);
size_t get_max(struct node *x, struct node *y)
{
    size_t xs = ALLOCED_BLOB(x) ? 0 : x->max_size;
    size_t ys = ALLOCED_BLOB(y) ? 0 : y->max_size;
    return xs > ys ? xs : ys;
}


static struct node *create_node(struct paging_state *st);
static struct node *create_node(struct paging_state *st)
{
    struct node *n = st->next_node++;
    n->max_size = 0;
    n->addr = 0;
    n->allocated = false;
    n->left = NULL;
    n->right = NULL;
    return n;
}

void remove_node(struct node *n);
void remove_node(struct node *n){
    // TODO deallocate from heap and rearrange
}

/**
 * Allocate memory
 */
lvaddr_t buddy_alloc(struct paging_state *st,
                     struct node *cur, size_t req_size);
lvaddr_t buddy_alloc(struct paging_state *st,
                     struct node *cur, size_t req_size)
{   
    // Pre-conditions
    assert((!cur->left && !cur->right) || (cur->left && cur->right));
    
    // Size available in subtree is less than what's requested
    if (cur->max_size < req_size){
        return -1;
    }
        
    // Case 1: Node is a leaf (i.e. blob of memory)
    if (!cur->left){
        assert(!cur->allocated);
        
        // Case 1a: Blob is big enough to do a split and recurse
        if ((cur->max_size >> 1) > req_size &&
            (cur->max_size >> 1) >= MIN_BLOB_SIZE) {

            size_t half_size = cur->max_size >> 1;
                
            struct node *left_buddy = create_node(st);
            left_buddy->allocated = false;
            left_buddy->addr = cur->addr;
            left_buddy->max_size = half_size;
            
            struct node *right_buddy = create_node(st);
            right_buddy->allocated = false;
            right_buddy->addr = cur->addr ^ half_size;
            right_buddy->max_size = half_size;
            
            cur->left = left_buddy;
            cur->right = right_buddy;
            cur->max_size = half_size;
            cur->allocated = true;
            
            // Sanity checks
            assert(left_buddy->addr < right_buddy->addr);
            assert((left_buddy->addr ^ right_buddy->addr) == half_size);
            
            return buddy_alloc(st, left_buddy, req_size);
            
        // Case 1b: Blob is best fit, so mark as allocated and return address
        } else {
            /*
            size_t pre_free, pre_alloc, post_free, post_alloc;
            debug_get_free_space(&pre_free, &pre_alloc);
            */
            
            assert(&current == st);
            assert(cur->max_size > 0);

            cur->allocated = true;
            
            /*
            debug_get_free_space(&post_free, &post_alloc);
            assert(pre_free - cur->max_size == post_free);
            assert(pre_alloc + cur->max_size == post_alloc);
            assert(pre_free + pre_alloc == post_free + post_alloc);
            */
            
            return cur->addr;
        }

    // Case 2: Node is intermediate, so locate the child that fits the best
    } else {
        struct node *best_fit;
        
        bool left_too_small = cur->left->max_size < req_size;
        bool right_fits_better =
            ((cur->left->max_size > cur->right->max_size) &&
            (cur->right->max_size >= req_size));
        
        if (ALLOCED_BLOB(cur->left) || left_too_small || right_fits_better){
            if(cur->right->max_size == 0){
                return -1;
            }
            best_fit = cur->right;
        } else {
            // Sanity check
            assert(cur->left->max_size);
            
            best_fit = cur->left;
        }

        // Recursively find for appropriate node and
        // update max_size value
        lvaddr_t addr = buddy_alloc(st, best_fit, req_size);
        cur->max_size = get_max(cur->left, cur->right);
        
        // Post conditions
        assert((!cur->left && !cur->right) || (cur->left && cur->right));
        assert(ALLOCED_BLOB(cur->left) || cur->max_size >= cur->left->max_size);
        assert(ALLOCED_BLOB(cur->right) || cur->max_size >= cur->right->max_size);
        
        return addr;
    }    
}

static bool buddy_check_addr(struct node *cur, lvaddr_t addr)
{
    // We're either a leaf or have two children
    assert((!cur->left && !cur->right) || (cur->left && cur->right));
    
    // If cur is a leaf, addr must be within range
    if (!cur->left){
        return cur->allocated &&
               cur->addr <= addr &&
               cur->addr+cur->max_size > addr;
    
    // Otherwise we recurse
    } else {
        struct node *next =
            (cur->right->addr <= addr) ? cur->right : cur->left;
        return buddy_check_addr(next, addr);
    }
}

static size_t debug_get_free_space_aux(struct node *cur)
{
    assert(cur);
    assert((cur->left && cur->right) || (!cur->left && !cur->right));
    
    size_t size = 0;
    if(cur->left) {
        size = debug_get_free_space_aux(cur->left)
               + debug_get_free_space_aux(cur->right);
    } else if(!cur->allocated) {
        size = cur->max_size;
    }
    
    return size;
}

static size_t debug_get_alloc_space_aux(struct node *cur)
{
    assert(cur);
    assert((cur->left && cur->right) || (!cur->left && !cur->right));
    
    size_t size = 0;
    if(cur->left) {
        size = debug_get_alloc_space_aux(cur->left)
               + debug_get_alloc_space_aux(cur->right);
    } else if(cur->allocated) {
        size = cur->max_size;
    }
    
    return size;
}

void debug_get_free_space(size_t *free_s, size_t *alloc_s, size_t *max_s)
{
    *free_s = debug_get_free_space_aux(current.root);
    *alloc_s = debug_get_alloc_space_aux(current.root);
    *max_s = current.root->max_size;
}

static void buddy_dealloc(struct node *cur, lvaddr_t addr) 
{
    assert(cur);
    assert((cur->left && cur->right) || (!cur->left && !cur->right));

    if (!cur->left) {
        // We're a leaft
        
        if (cur->allocated) {
            assert(cur->addr == addr);
            cur->allocated = false;
            return;
        } else  {
            return;
        }
    } else {
        // We're intermediate node
        
        if (cur->right->addr <= addr) {
            buddy_dealloc(cur->right, addr);
        } else {
            buddy_dealloc(cur->left, addr);
        }    
    }
    
    // Sanity check
    assert(cur->left && cur->right);
    
    // If possible, coerce child nodes
    if (!cur->left->allocated && !cur->right->allocated){
        size_t new_size = cur->addr ^ cur->right->addr;
        
        remove_node(cur->left);
        remove_node(cur->right);
        
        cur->left = NULL;
        cur->right = NULL;
        cur->allocated = false;
        cur->max_size = new_size;
    } else {
        cur->max_size = get_max(cur->left, cur->right);
    }
}

uint32_t stack[EXC_STACK_SIZE];

static errval_t allocate_pt(struct paging_state *st, lvaddr_t addr,
                            struct capref frame_cap, size_t start_offset,
                            size_t bytes, int flags)
{
    errval_t err = SYS_ERR_OK;

    // Relevant pagetable slots
    cslot_t l1_slot = ARM_L1_OFFSET(addr)>>2;

    uint32_t l1_entries =
        DIVIDE_ROUND_UP(bytes, BASE_PAGE_SIZE*(ARM_L1_MAX_ENTRIES>>2));

    // Check if we have l2 capabilities and request if not
    size_t l2_entries_mapped = 0;
    for(uint32_t i = 0; i < l1_entries; i++, l1_slot++){

        // In each new iteration we need to make a copy of the
        // capability before mapping
        if (i > 0) {
            struct capref copy;
            err = devframe_type(&copy, frame_cap, log2ceil(bytes));
            if (err_is_fail(err)) {
                debug_printf("Could not copy capref: %s\n", err_getstring(err));
                err_print_calltrace(err);
                return err;
            } else {
                frame_cap = copy;
            }
        }

        lvaddr_t next_addr = addr+l2_entries_mapped*BASE_PAGE_SIZE;
        lvaddr_t next_bytes = bytes-l2_entries_mapped*BASE_PAGE_SIZE;
        size_t cap_offset = l2_entries_mapped*BASE_PAGE_SIZE+start_offset;

        // Sanity checks
        assert(next_addr < addr+bytes);
        assert(0 < next_bytes && next_bytes <= bytes);

        // Next starting l2-slot
        cslot_t l2_slot =
            ARM_L2_OFFSET(next_addr)+(ARM_L1_OFFSET(next_addr)%4)*ARM_L2_MAX_ENTRIES;

        // Number of entries in the L2-pagetable
        uint32_t pages = DIVIDE_ROUND_UP(next_bytes, BASE_PAGE_SIZE);
        uint32_t l2_entries = ROUND_UP(pages, ENTRIES_PER_FRAME);
        l2_entries = MIN(l2_entries, (ARM_L2_MAX_ENTRIES<<2)-l2_slot);


        // // TODO remove
        // debug_printf("next_bytes: %d\n", next_bytes);
        // debug_printf("next_addr: 0x%08x\n", next_addr);
        // debug_printf("l1_slot: %d\n", l1_slot);
        // debug_printf("l2_slot: %d\n", l2_slot);
        // debug_printf("pages: %d\n", pages);
        // debug_printf("l1_entries: %d\n", l1_entries);
        // debug_printf("l2_entries: %d\n", l2_entries);
        // debug_printf("cap_offset: %d\n", cap_offset);


        // Allocate and insert l2-capability
        struct capref *l2_cap = &(st->l2_caps[l1_slot]);
        if(capref_is_null(*l2_cap)) {
            // Allocate capability
            arml2_alloc(l2_cap);

            // Insert L2 pagetable in L1 pagetable
            err = vnode_map(st->l1_cap, *l2_cap, l1_slot, flags, 0, 1);

            if (err_is_fail(err)) {
                debug_printf("Could not insert L2 pagetable in L1 pagetable for addr 0x%08x: %s\n", addr, err_getstring(err));
                err_print_calltrace(err);
                return err;
            }
        }

        // Map frame/dev capability in L2 pagetable
        err = vnode_map(*l2_cap, frame_cap, ROUND_DOWN(l2_slot, ENTRIES_PER_FRAME),
                        flags, cap_offset, l2_entries);
        if (err_is_fail(err)){
            debug_printf("Could not insert frame in L2 pagetable for addr 0x%08x: %s\n",
                         addr, err_getstring(err));
            err_print_calltrace(err);
            return err;
        }

        l2_entries_mapped += l2_entries;
    }

    return err;
}

errval_t paging_map_device(struct paging_state *st, lvaddr_t addr,
                            struct capref frame_cap, uint64_t start_offset, 
                            size_t length, int flags) 
{
    return allocate_pt(st, addr, frame_cap, start_offset, length, flags);
}

void page_fault_handler(enum exception_type type, int subtype,
                        void *addr, arch_registers_state_t *regs,
                        arch_registers_fpu_state_t *fpuregs);

void page_fault_handler(enum exception_type type, int subtype,
                        void *addr, arch_registers_state_t *regs,
                        arch_registers_fpu_state_t *fpuregs)
{
    lvaddr_t vaddr = (lvaddr_t)addr;
    
    // We assume all structs are less than 1KB, so pagefaults on addrs
    // less than this count as NULL pointer exception
    type = (vaddr < MAX_STRUCT_SIZE) ? EXCEPT_NULL
         : (vaddr >= VADDR_OFFSET - BASE_PAGE_SIZE) ? EXCEPT_OTHER
         : type;
    
    // Handle non-pagefault exceptions
    switch(type){
        case EXCEPT_NULL:
            // TODO print
            exit(-1);
        case EXCEPT_BREAKPOINT:
        case EXCEPT_SINGLESTEP:
            // TODO stuff
            return;
        case EXCEPT_OTHER:
            // TODO print
            exit(-1);
        default:;
    }
        
    // Check if addr is in buddy allocation tree and throw error if not
    if (!buddy_check_addr(current.root, (lvaddr_t)addr)) {
        debug_printf("Did not find address 0x%08x!\n", (lvaddr_t)addr);
        // buddy_dealloc(current.root, 0);
        // TODO print
        exit(-1);
    }
    
    /* Otherwise we need to allocate. If we're init, we do it directly,
       and otherwise we do it by RPC */
    struct capref frame_cap = NULL_CAP;
    size_t req_size = BASE_PAGE_SIZE*ENTRIES_PER_FRAME;
    size_t ret_size;
    errval_t err;
   
    const char *obj = "init";
    const char *prog = disp_name();

    if(strlen(prog) == 4 && strncmp(obj, prog, 4) == 0){
        err = frame_alloc(&frame_cap, req_size, &ret_size);
    } else {
        size_t req_bits = log2ceil(req_size);
        size_t ret_bits;
        err = aos_rpc_get_ram_cap(current.rpc, req_bits, &frame_cap, &ret_bits);
        ret_size = (1UL << ret_bits);
    }
    
    if (err != SYS_ERR_OK){
        debug_printf("Could not allocate frame for addr 0x%08x: %s\n",
                     addr, err_getstring(err));
        exit(-1);
    }

    if (ret_size != req_size){
        debug_printf("Tried to allocate %d bytes for addr 0x%08x\
            but could only allocate %d.\n",
                     req_size, addr, ret_size);
        exit(-1);
    }
     
    // Allocate L1 and L2 entries, if needed, and insert frame cap
    allocate_pt(&current, (lvaddr_t)addr, frame_cap,
                0, ret_size, VREGION_FLAGS_READ_WRITE);
}

errval_t paging_init_state(struct paging_state *st, lvaddr_t start_vaddr,
        struct capref pdir)
{
    debug_printf("paging_init_state\n");
    
    start_vaddr = MAX(start_vaddr, V_OFFSET);
    st->l1_cap = pdir;
    current = *st;
    
    for(uint32_t i = 0; i < ARM_L1_USER_ENTRIES; i++){
        st->l2_caps[i] = NULL_CAP;
    }
    
    // for(uint32_t i = 0; i < NO_OF_FRAMES; i++){
    //     st->frame_caps[i] = NULL_CAP;
    // }
    
    // st->guard_cap = NULL_CAP;

    st->next_node = st->all_nodes;
    // st->next_frame = st->frame_caps;
    st->root = create_node(st);
    st->root->max_size = (1UL << 31); // TODO subtract stack size
    st->root->addr = 0;
    
    buddy_alloc(st, st->root, V_OFFSET);
        
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

    // TIP: it might be a good idea to call paging_init_state() from here to
    // avoid code duplication.
    struct capref l1_cap = {
        .cnode = cnode_page,
        .slot = 0, 
    };

    set_current_paging_state(&current);
    paging_init_state(&current, 0, l1_cap);
    
    return SYS_ERR_OK;
}

void paging_init_onthread(struct thread *t)
{
    // TODO: setup exception handler for thread `t'.
    void *buf;
    errval_t err = paging_alloc(&current, &buf, EXC_STACK_SIZE);
    if (err_is_fail(err)) {
        debug_printf("Could not allocate exception stack for new thread: %s\n",
            err_getstring(err));
        err_push(err, LIB_ERR_MALLOC_FAIL);
        err_print_calltrace(err);
        t = NULL;
        return;
    }
    
    t->exception_stack = buf;
    t->exception_stack_top = buf+EXC_STACK_SIZE*4; // TODO minus 1 here?
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
 */
errval_t paging_alloc(struct paging_state *st, void **buf, size_t bytes)
{
    if(!st){
        debug_printf("arg st is NULL");
        exit(-1);
    }

    current = *st;

    // find virtual address from AVL-tree
    *((lvaddr_t*)buf) = buddy_alloc(st, st->root, bytes);
    
    if(*buf == (void*)-1) {
        debug_printf("Could not allocate space\n");
        exit(-1);
    }
    
    // TODO handle error
    return SYS_ERR_OK;
}

/**
 * \brief 
 */
errval_t paging_dealloc(struct paging_state *st, void *buf)
{
    buddy_dealloc(st->root, (lvaddr_t)buf);
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
        debug_printf("paging_map_frame_attr: Could not allocate space for frame cap: %s\n",
            err_getstring(err));
        err_push(err, LIB_ERR_MALLOC_FAIL);
        err_print_calltrace(err);
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
    
    errval_t err = allocate_pt(st, vaddr, frame, 0, bytes, flags);
        
    if (err_is_fail(err)){
        debug_printf("Could not map fixed attribute: %s", err_getstring(err));
        err_push(err, LIB_ERR_MALLOC_FAIL);
        err_print_calltrace(err);
    }
    
    
    
    return SYS_ERR_OK;
    // return err;
}

/**
 * \brief unmap a user provided frame, and return the VA of the mapped
 *        frame in `buf`.
 * NOTE: this function is currently here to make libbarrelfish compile. As
 * noted on paging_region_unmap we ignore unmap requests right now.
 */
errval_t paging_unmap(struct paging_state *st, const void *region)
{
    buddy_dealloc(st->root, (lvaddr_t)region);
    return SYS_ERR_OK;
}
