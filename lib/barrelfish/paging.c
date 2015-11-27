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
// minimum size to allocate
#define MIN_BLOB_SIZE (ENTRIES_PER_FRAME*BASE_PAGE_SIZE) 

#define PRINT_CALLS 0

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

static size_t get_max(struct node *x, struct node *y);
static size_t get_max(struct node *x, struct node *y)
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
static lvaddr_t buddy_alloc(struct paging_state *st,
                            struct node *cur, size_t req_size);
static lvaddr_t buddy_alloc(struct paging_state *st,
                            struct node *cur, size_t req_size)
{   
    // Pre-conditions
    assert(st);
    assert((!cur->left && !cur->right) || (cur->left && cur->right));
    
    // Size available in subtree is less than what's requested
    if (cur->max_size < req_size){
        abort();
        return -1;
    }
    
    if((lvaddr_t)cur == 0x0161d8f0){
        debug_printf("node 0x0161d8f0, "
                     "max size: %d\n", cur->max_size);
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
            
            if((lvaddr_t)left_buddy == 0x0161d8f0){
                debug_printf("left_buddy 0x0161d8f0, "
                             "max size: %d\n", left_buddy->max_size);
            }
            
            if((lvaddr_t)right_buddy == 0x0161d8f0){
                debug_printf("right_buddy 0x0161d8f0, "
                             "max size: %d\n", right_buddy->max_size);
            }
            
            // Sanity checks
            assert(left_buddy->addr < right_buddy->addr);
            assert((left_buddy->addr ^ right_buddy->addr) == half_size);
            assert(left_buddy->max_size >= MIN_BLOB_SIZE);
            assert(right_buddy->max_size >= MIN_BLOB_SIZE);
            
            return buddy_alloc(st, left_buddy, req_size);
            
        // Case 1b: Blob is best fit, so mark as allocated and return address
        } else {
            /*
            size_t pre_free, pre_alloc, post_free, post_alloc;
            debug_get_free_space(&pre_free, &pre_alloc);
            */
            
            assert(cur->max_size > MIN_BLOB_SIZE);
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
            // Sanity check
            assert(cur->right->max_size >= MIN_BLOB_SIZE);

            best_fit = cur->right;
        } else {
            // Sanity check
            if(cur->left->max_size < MIN_BLOB_SIZE){
                debug_printf("cur->left addr: 0x%08x\n", cur->left);
            }
            
            assert(cur->left->max_size >= MIN_BLOB_SIZE);
            
            best_fit = cur->left;
        }

        // Recursively find for appropriate node and
        // update max_size value
        lvaddr_t addr = buddy_alloc(st, best_fit, req_size);
        cur->max_size = get_max(cur->left, cur->right);
        
        // Post conditions
        assert((!cur->left && !cur->right) || (cur->left && cur->right));
        assert(ALLOCED_BLOB(cur->left) || cur->max_size >= cur->left->max_size);
        assert(ALLOCED_BLOB(cur->right) || cur->max_size >= 
            cur->right->max_size);
        
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
#if PRINT_CALLS
    debug_printf("debug_get_free_space called\n");
#endif
    
    *free_s = debug_get_free_space_aux(current.root);
    *alloc_s = debug_get_alloc_space_aux(current.root);
    *max_s = current.root->max_size;

#if PRINT_CALLS
    debug_printf("debug_get_free_space returned\n");
#endif
}

static size_t buddy_dealloc(struct node *cur, lvaddr_t addr) 
{
    assert(cur);
    assert((cur->left && cur->right) || (!cur->left && !cur->right));

    size_t return_size;

    if (!cur->left) {
        // We're a leaf
        
        if (cur->allocated) {
            assert(cur->addr == addr);
            cur->allocated = false;
            return cur->max_size;
        } else {
            abort();
            return 0;
        }
    } else {
        // We're intermediate node
        
        if (cur->right->addr <= addr) {
            return_size = buddy_dealloc(cur->right, addr);
        } else {
            return_size = buddy_dealloc(cur->left, addr);
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
    
    return return_size;
}

uint32_t stack[EXC_STACK_SIZE];

static errval_t allocate_pt(struct paging_state *st, lvaddr_t addr,
                            struct capref frame_cap, size_t start_offset,
                            size_t bytes, int flags, bool align)
{
    errval_t err = SYS_ERR_OK;
    
    // Relevant pagetable slots
    cslot_t l1_slot = ARM_L1_USER_OFFSET(addr);

    uint32_t l1_entries =
        DIVIDE_ROUND_UP(bytes, BASE_PAGE_SIZE * ARM_L1_USER_ENTRIES);

    // Check if we have l2 capabilities and request if not
    size_t l2_entries_mapped = 0;
    for(uint32_t i = 0; i < l1_entries; i++, l1_slot++){

        // In each new iteration we need to retype the
        // capability before mapping
        if (i > 0) {
            
            // more generic copying I suppose? 
            struct capref newcap;
            slot_alloc(&newcap);
            err = cap_retype(newcap, frame_cap, ObjType_Frame, log2ceil(bytes));
            frame_cap = newcap;

            // KEEP THIS, WE MIGHT NEED IT LATER!
            // struct capref copy;
            // // err = devframe_type(&copy, frame_cap, log2ceil(bytes));
            // err = invoke_frame_identify(frame_cap, &copy);
            // assert(err_is_ok(err));
            //
            // if (err_is_fail(err)) {
            //     debug_printf("Could not copy capref: %s\n", err_getstring(err));
            //     err_print_calltrace(err);
            //     return err;
            // } else {
            //     frame_cap = copy;
            // }
        }

        lvaddr_t next_addr = addr + l2_entries_mapped * BASE_PAGE_SIZE;
        lvaddr_t next_bytes = bytes - l2_entries_mapped * BASE_PAGE_SIZE;
        size_t cap_offset = l2_entries_mapped * BASE_PAGE_SIZE + start_offset;        
        
        // Sanity checks
        assert(next_addr < addr+bytes);
        assert(0 < next_bytes && next_bytes <= bytes);

        // Next starting l2-slot
        cslot_t l2_slot = ARM_L2_USER_OFFSET(next_addr);
        cslot_t l2_slot_aligned =
            align ? ROUND_DOWN(l2_slot, ENTRIES_PER_FRAME) : l2_slot;

        // Number of entries in the L2-pagetable
        uint32_t pages = DIVIDE_ROUND_UP(next_bytes, BASE_PAGE_SIZE);
        uint32_t l2_entries = align ? ROUND_UP(pages, ENTRIES_PER_FRAME) : 
            pages;
        l2_entries = MIN(l2_entries, ARM_L2_USER_ENTRIES - l2_slot);

        // TODO remove

#if 0
        // debug_printf("proxy MEM(0x%08x,%d,%d,%d,%d)",
            // next_addr, next_bytes, l1_slot, l2_slot_aligned, l2_entries);
        debug_printf("st:                0x%08x\n", st);
        debug_printf("next_addr:         0x%08x\n", next_addr);
        debug_printf("next_bytes:        0x%08x\n", next_bytes);
        debug_printf("next_cap_offset:   0x%08x\n", cap_offset);
        debug_printf("l1_slot:           %d\n",     l1_slot);
        debug_printf("l2_slot:           %d\n",     l2_slot);
        debug_printf("l2_slot_aligned:   %d\n",     l2_slot_aligned);
        debug_printf("pages:             %d\n",     pages);
        debug_printf("l2_entries:        %d\n",     l2_entries);
        debug_printf("l2_entries_mapped: %d\n\n",   l2_entries_mapped);
        debug_printf("align:             %d\n\n",   align);
#endif
        // Allocate and insert l2-capability
        struct capref *l2_cap = &(st->l2_caps[l1_slot]);
        if(capref_is_null(*l2_cap)){
            // Allocate capability
            arml2_alloc(l2_cap);

            // Insert L2 pagetable in L1 pagetable
            // debug_printf("L1-MAPPING: l1: %d, l2 %d, addr 0x%08x for st 0x%08x\n",
            //     l1_slot, l2_slot_aligned, next_addr, st);
                
            err = vnode_map(st->l1_cap, *l2_cap, l1_slot, flags, 0, 1);
            if (err_is_fail(err)) {
                debug_printf("Could not insert L2 pagetable in L1 pagetable "
                    "for addr 0x%08x: %s\n", addr, err_getstring(err));
                err_print_calltrace(err);
                return err;
            }
            
            // debug_printf("\n");
            // debug_printf("Mapped l1-entries for 0x%08x:\n", st);
            // for(uint32_t j = 0; j < ARM_L1_USER_ENTRIES; j++){
            //     if(!capref_is_null(st->l2_caps[j])){
            //         debug_printf("slot %d\n", j);
            //     }
            // }
            // debug_printf("\n");
        }

        //Map frame/dev capability in L2 pagetable
        if(l1_slot == 0)
            debug_printf("L2-MAPPING: l1: %d, l2: %d, entries: %d, addr: 0x%08x\n",
                l1_slot, l2_slot_aligned, l2_entries, next_addr);
        err = vnode_map(*l2_cap, frame_cap, l2_slot_aligned,
                        flags, cap_offset, l2_entries);
        if (err_is_fail(err)) {
            debug_printf("Could not insert frame in L2 pagetable for" 
                " addr 0x%08x: %s\n", addr, err_getstring(err));
            err_print_calltrace(err);
            return err;
        }
        l2_entries_mapped += l2_entries;
    }

    return err;
}
                            
errval_t paging_map_user_device(struct paging_state *st, lvaddr_t addr,
                            struct capref frame_cap, uint64_t start_offset, 
                            size_t length, int flags) 
{
#if PRINT_CALLS
    debug_printf("paging_map_user_device called for st 0x%08x, addr 0x%08x\n",
        st, addr);
#endif

    errval_t err;
    
    cslot_t l1_slot = ARM_L1_USER_OFFSET(addr);
    cslot_t l2_slot = ARM_L2_USER_OFFSET(addr);
    uint32_t l2_entries = DIVIDE_ROUND_UP(length, BASE_PAGE_SIZE);

    struct capref *l2_cap = &(st->l2_caps[l1_slot]);
    if(capref_is_null(*l2_cap)) {
        // Allocate capability
        arml2_alloc(l2_cap);

        // Insert L2 pagetable in L1 pagetable
        err = vnode_map(st->l1_cap, *l2_cap, l1_slot,
                        VREGION_FLAGS_READ_WRITE, 0, 1);

        if (err_is_fail(err)) {
            debug_printf("Could not insert L2 pagetable in L1 pagetable"
                         " for addr 0x%08x: %s\n", addr, err_getstring(err));
            err_print_calltrace(err);
            return err;
        }
    }
    
    
    // debug_printf("map_device: l1: %d, l2: %d, entries: %d\n", l1_slot, l2_slot, l2_entries);
    err = vnode_map(*l2_cap, frame_cap, l2_slot,
                    flags, start_offset, l2_entries);
    if (err_is_fail(err)){
        debug_printf("Could not insert dev cap in L2 pagetable"
                     " for addr 0x%08x: %s\n", addr, err_getstring(err));
        err_print_calltrace(err);
        return err;
    }
#if PRINT_CALLS    
    debug_printf("paging_map_user_device returned\n");
#endif
    return SYS_ERR_OK;
}

void page_fault_handler(enum exception_type type, int subtype,
                        void *addr, arch_registers_state_t *regs,
                        arch_registers_fpu_state_t *fpuregs);

void page_fault_handler(enum exception_type type, int subtype,
                        void *addr, arch_registers_state_t *regs,
                        arch_registers_fpu_state_t *fpuregs)
{
#if PRINT_CALLS
    debug_printf("page_fault_handler called for addr 0x%08x\n", addr);
#endif
    
    lvaddr_t vaddr = (lvaddr_t)addr;
    // We assume all structs are less than 1KB, so pagefaults on addrs
    // less than this count as NULL pointer exception
    long long unsigned limit = (1ULL << 32) - BASE_PAGE_SIZE;
    type = (vaddr < MAX_STRUCT_SIZE) ? EXCEPT_NULL
         : (vaddr >= limit) ? EXCEPT_OTHER
         : type;
    
    // Handle non-pagefault exceptions
    switch(type){
        case EXCEPT_NULL:
            debug_printf("Hit null pointer at addr 0x%08x!\n", addr);
            abort();
        case EXCEPT_BREAKPOINT:
        case EXCEPT_SINGLESTEP:
            debug_printf("Debugging control\n");
            return;
        case EXCEPT_OTHER:
            debug_printf("Other exception...\n");
            abort();
        default:;
    }
    
    // Check if addr is in buddy allocation tree and throw error if not
    if (!buddy_check_addr(current.root, (lvaddr_t)addr)) {
        debug_printf("Did not find address 0x%08x!\n", (lvaddr_t)addr);
        abort();        
    }
    
    /* Otherwise we need to allocate. If we're init, we do it directly,
       and otherwise we do it by RPC */
    struct capref frame_cap = NULL_CAP;
    size_t req_size = BASE_PAGE_SIZE * ENTRIES_PER_FRAME;
    size_t ret_size;
    errval_t err;
   
    const char *obj = "init";
    const char *prog = disp_name();
    bool is_init = strlen(prog) == 4 && strncmp(obj, prog, 4) == 0;
    if(is_init){
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
        abort();
    }

    if (ret_size != req_size){
        debug_printf("Tried to allocate %d bytes for addr 0x%08x\
            but could only allocate %d.\n",
                     req_size, addr, ret_size);
        abort();
    }
     
    // Allocate L1 and L2 entries, if needed, and insert frame cap
    allocate_pt(&current, (lvaddr_t)addr, frame_cap,
                0, ret_size, VREGION_FLAGS_READ_WRITE, true);
                
#if PRINT_CALLS
    debug_printf("page_fault_handler returned\n");
#endif
}

errval_t paging_init_state(struct paging_state *st, lvaddr_t start_vaddr,
        struct capref pdir)
{
#if PRINT_CALLS
    debug_printf("paging_init_state called for st: 0x%08x\n", st);
#endif
        
    start_vaddr = MAX(start_vaddr, V_OFFSET);
    st->l1_cap = pdir;
    
    for(uint32_t i = 0; i < ARM_L1_USER_ENTRIES; i++){
        st->l2_caps[i] = NULL_CAP;
    }
    
    st->next_node = st->all_nodes;
    st->root = create_node(st);
    st->root->max_size = (1ULL<<32); // TODO subtract stack size
    st->root->addr = 0;

    buddy_alloc(st, st->root, MAX(V_OFFSET, start_vaddr-1));
    
#if PRINT_CALLS
    debug_printf("paging_init_state returned\n");
#endif
    return SYS_ERR_OK;
}


errval_t paging_init(void)
{
#if PRINT_CALLS
    debug_printf("paging_init with current: 0x%08x\n", &current);
#endif
    
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

    const char *obj = "init";
    const char *prog = disp_name();
    bool is_init = strlen(prog) == 4 && strncmp(obj, prog, 4) == 0;
    
    lvaddr_t start_addr = is_init ? 0 : (1UL<<30);
    
    set_current_paging_state(&current);
    paging_init_state(&current, start_addr, l1_cap);
    
#if PRINT_CALLS
    debug_printf("paging_init returned\n");
#endif
    return SYS_ERR_OK;
}

void paging_init_onthread(struct thread *t)
{
#if PRINT_CALLS
    debug_printf("paging_init_onthread called for thread 0x%08x\n", t);
#endif

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
    
    page_fault_handler(EXCEPT_PAGEFAULT, 0, buf, NULL, NULL);
    
    t->exception_stack = buf;
    t->exception_stack_top = buf+EXC_STACK_SIZE*4; // TODO minus 1 here?
    t->exception_handler = page_fault_handler;
#if PRINT_CALLS
    debug_printf("paging_init_onthread returned\n");
#endif
}

/**
 * \brief return a pointer to a bit of the paging region `pr`.
 * This function gets used in some of the code that is responsible
 * for allocating Frame (and other) capabilities.
 */
errval_t paging_region_init(struct paging_state *st, struct paging_region *pr, 
    size_t size)
{
#if PRINT_CALLS
    debug_printf("paging_region_init called for st 0x%08x\n", st);
#endif
    void *base;
    errval_t err = paging_alloc(st, &base, size);
    if (err_is_fail(err)) {
        debug_printf("paging_region_init: paging_alloc failed\n");
        return err_push(err, LIB_ERR_VSPACE_MMU_AWARE_INIT);
    }
    pr->base_addr    = (lvaddr_t)base;
    pr->current_addr = pr->base_addr;
    pr->region_size  = size;

#if PRINT_CALLS
    debug_printf("paging_region_init returned\n");
#endif
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
#if PRINT_CALLS
    debug_printf("paging_region_map called for region 0x%08x"
                 ", req_size: %d\n", pr, req_size);
#endif
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
#if PRINT_CALLS
    debug_printf("paging_region_map returned addr 0x%08x, ret_size: %d\n",
        (lvaddr_t)*retbuf, *ret_size);
#endif
    return SYS_ERR_OK;
}

/**
 * \brief free a bit of the paging region `pr`.
 * This function gets used in some of the code that is responsible
 * for allocating Frame (and other) capabilities.
 * We ignore unmap requests right now.
 */
errval_t paging_region_unmap(struct paging_region *pr, lvaddr_t base, 
    size_t bytes)
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
#if PRINT_CALLS
    debug_printf("paging_alloc called for st 0x%08x, bytes: %d\n",
        st, bytes);
#endif
    
    if(bytes == 0){
        return SYS_ERR_VM_MAP_SIZE;
    }
    
    assert(st != NULL);
    current = *st;
    bytes = ROUND_UP(bytes, MIN_BLOB_SIZE);
    
    // find virtual address from AVL-tree
    *((lvaddr_t*)buf) = buddy_alloc(st, st->root, bytes);

    if(*buf == (void*)-1) {
        debug_printf("Could not allocate space\n");
        exit(-1);
    }
    assert((lvaddr_t)*buf >= V_OFFSET);

    // TODO handle error
#if PRINT_CALLS
    debug_printf("paging_alloc returned 0x%08x\n", (lvaddr_t) *buf);
#endif
    return SYS_ERR_OK;
}

/**
 * \brief 
 */
errval_t paging_dealloc(struct paging_state *st, void *buf)
{
#if PRINT_CALLS
    debug_printf("paging_dealloc called\n");
#endif
    buddy_dealloc(st->root, (lvaddr_t)buf);
#if PRINT_CALLS
    debug_printf("paging_dealloc returne\n");
#endif
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
#if PRINT_CALLS
    debug_printf("paging_map_frame_attr called for st 0x%08x\n", st);
#endif

    assert(!capref_is_null(frame));
    bytes = ROUND_UP(bytes, BASE_PAGE_SIZE*ENTRIES_PER_FRAME);
    *((lvaddr_t*)buf) = buddy_alloc(st, st->root, bytes);
    if(*buf == (void*)-1) {
        debug_printf("Could not allocate space\n");
        exit(-1);
    }
    
#if PRINT_CALLS
    debug_printf("paging_map_frame_attr returning\n");
#endif
    return paging_map_fixed_attr(st, (lvaddr_t)(*buf), frame, bytes, flags);
}


/**
 * \brief map a user provided frame at user provided VA.
 */
errval_t paging_map_fixed_attr(struct paging_state *st, lvaddr_t vaddr,
        struct capref frame, size_t bytes, int flags)
{
#if PRINT_CALLS
    debug_printf("paging_map_fixed_attr called st 0x%08x, bytes %d\n",
        st, bytes);
#endif
    
    struct capref capcopy;
    slot_alloc(&capcopy);
    errval_t err = cap_copy(capcopy, frame);
    if (err_is_fail(err)){
        err_print_calltrace(err);
        abort();
    }
    
    err = allocate_pt(st, vaddr, capcopy, 0, bytes, flags, false);
    if (err_is_fail(err)){
        debug_printf("Could not map fixed attribute: %s\n", err_getstring(err));
        err_push(err, LIB_ERR_MALLOC_FAIL);
        err_print_calltrace(err);
    }

    // LEGACY CODE, LETS KEEP IT FOR NOW
    // errval_t err;
    // cslot_t l1_slot = ARM_L1_USER_OFFSET(vaddr);
    // cslot_t l2_slot = ARM_L2_USER_OFFSET(vaddr);
    // size_t entries = DIVIDE_ROUND_UP(bytes, BASE_PAGE_SIZE);
    // struct capref *l2_cap = &(st->l2_caps[l1_slot]);
    // if(capref_is_null(*l2_cap)) {
    //     // Allocate capability
    //     arml2_alloc(l2_cap);
    //
    //     // Insert L2 pagetable in L1 pagetable
    //     err = vnode_map(st->l1_cap, *l2_cap, l1_slot,
    //                     VREGION_FLAGS_READ_WRITE, 0, 1);
    //
    //     if (err_is_fail(err)) {
    //         debug_printf("Could not insert L2 pagetable in L1 pagetable"
    //                      " for addr 0x%08x: %s\n", vaddr, err_getstring(err));
    //         err_print_calltrace(err);
    //         return err;
    //     }
    // }
    //
    //
    // err = vnode_map(*l2_cap, frame, l2_slot,
    //                 flags, 0, entries);
    // if (err_is_fail(err)){
    //     debug_printf("Could not insert dev cap in L2 pagetable"
    //                  " for addr 0x%08x: %s\n", vaddr, err_getstring(err));
    //     err_print_calltrace(err);
    //     return err;
    // }

#if PRINT_CALLS
    debug_printf("paging_map_fixed_attr returned\n");
#endif
    return err;
}

/**
 * \brief unmap a user provided frame, and return the VA of the mapped
 *        frame in `buf`.
 * NOTE: this function is currently here to make libbarrelfish compile. As
 * noted on paging_region_unmap we ignore unmap requests right now.
 */
errval_t paging_unmap(struct paging_state *st, const void *region)
{
#if PRINT_CALLS
    debug_printf("paging_unmap called\n");
#endif
    
    lvaddr_t addr = (lvaddr_t) region;
    cslot_t l1_slot = ARM_L1_USER_OFFSET(addr);
    cslot_t l2_slot = ARM_L2_USER_OFFSET(addr);
    errval_t err = vnode_unmap(st->l1_cap, st->l2_caps[l1_slot], l2_slot, 1);
    if (err_is_fail(err)){
        err_print_calltrace(err);
        abort();
    }
    // size_t return_size = buddy_dealloc(st->root, (lvaddr_t)region);
    // debug_printf("unmap return_size: %d\n", return_size);
    
#if PRINT_CALLS
    debug_printf("paging_unmap returned\n");
#endif
    return SYS_ERR_OK;
}
