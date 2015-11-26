/**
 * \file
 * \brief functionality to spawn domains
 */

/*
 * Copyright (c) 2007, 2008, 2009, 2010, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <barrelfish/barrelfish.h>
#include <spawndomain/spawndomain.h>
#include <barrelfish/dispatcher_arch.h>
#include <elf/elf.h>
#include "spawn.h"
#include "../../arch.h"

#if defined(__arm__)
#define EM_HOST EM_ARM
#else
#error "Unexpected architecture."
#endif

struct monitor_allocate_state {
    void          *vbase;
    genvaddr_t     elfbase;
};


/**
 * \brief Convert elf flags to vregion flags
 */
static int elf_to_vregion_flags(uint32_t flags)
{
    int vregion_flags = 0;

    if (flags & PF_R) {
        vregion_flags |= VREGION_FLAGS_READ;
    }
    if (flags & PF_W) {
        vregion_flags |= VREGION_FLAGS_WRITE;
    }
    if (flags & PF_X) {
        vregion_flags |= VREGION_FLAGS_EXECUTE;
    }

    return vregion_flags;
}

static errval_t elf_allocate(void *state, genvaddr_t base, size_t size,
                             uint32_t flags, void **retbase)
{
    errval_t err;
    lvaddr_t vaddr;
    size_t used_size;

    struct spawninfo *si = state;

    // Increase size by space wasted on first page due to page-alignment
    size_t base_offset = BASE_PAGE_OFFSET(base);
    size += base_offset;
    base -= base_offset;
    // Page-align
    size = ROUND_UP(size, BASE_PAGE_SIZE);

    cslot_t vspace_slot = si->elfload_slot;

    // Step 1: Allocate the frames
    size_t sz = 0;
    for (lpaddr_t offset = 0; offset < size; offset += sz) {
        sz = 1UL << log2floor(size - offset);
        struct capref frame = {
            .cnode = si->segcn,
            .slot  = si->elfload_slot++,
        };
        err = frame_create(frame, sz, NULL);
        if (err_is_fail(err)) {
            return err_push(err, LIB_ERR_FRAME_CREATE);
        }
    }

    cslot_t spawn_vspace_slot = si->elfload_slot;
    cslot_t new_slot_count = si->elfload_slot - vspace_slot;

    // Step 2: create copies of the frame capabilities for child vspace
    for (int copy_idx = 0; copy_idx < new_slot_count; copy_idx++) {
        struct capref frame = {
            .cnode = si->segcn,
            .slot = vspace_slot + copy_idx,
        };

        struct capref spawn_frame = {
            .cnode = si->segcn,
            .slot = si->elfload_slot++,
        };
        err = cap_copy(spawn_frame, frame);
        if (err_is_fail(err)) {
            debug_printf("cap_copy failed for src_slot = %"PRIuCSLOT
                    ", dest_slot = %"PRIuCSLOT"\n", frame.slot,
                    spawn_frame.slot);
            return err_push(err, LIB_ERR_CAP_COPY);
        }
    }

    // Step 3: map into own vspace

    // Get virtual address range to hold the module
    void *vaddr_range;
    err = paging_alloc(get_current_paging_state(), &vaddr_range, size);
    if (err_is_fail(err)) {
        debug_printf("elf_allocate: paging_alloc failed\n");
        return (err);
    }

    // map allocated physical memory in virutal memory of parent process
    vaddr = (lvaddr_t)vaddr_range;
    used_size = size;

    while (used_size > 0) {
        struct capref frame = {
            .cnode = si->segcn,
            .slot  = vspace_slot++,
        };       // find out the size of the frame

        struct frame_identity id;
        err = invoke_frame_identify(frame, &id);
        assert(err_is_ok(err));
        size_t slot_size = (1UL << id.bits);

        // map frame to provide physical memory backing
        err = paging_map_fixed_attr(get_current_paging_state(), vaddr, frame, slot_size,
                VREGION_FLAGS_READ_WRITE);

        if (err_is_fail(err)) {
            debug_printf("elf_allocate: paging_map_fixed_attr failed\n");
            return err;
        }

        used_size -= slot_size;
        vaddr +=  slot_size;
    } // end while:


    // Step 3: map into new process
    struct paging_state *cp = si->vspace;

    // map allocated physical memory in virutal memory of child process
    vaddr = (lvaddr_t)base;
    used_size = size;

    while (used_size > 0) {
        struct capref frame = {
            .cnode = si->segcn,
            .slot  = spawn_vspace_slot++,
        };

        // find out the size of the frame
        struct frame_identity id;
        err = invoke_frame_identify(frame, &id);
        assert(err_is_ok(err));
        size_t slot_size = (1UL << id.bits);

        // map frame to provide physical memory backing
        err = paging_map_fixed_attr(cp, vaddr, frame, slot_size,
                elf_to_vregion_flags(flags));

        if (err_is_fail(err)) {
            debug_printf("elf_allocate: paging_map_fixed_attr failed\n");
            return err;
        }

        used_size -= slot_size;
        vaddr +=  slot_size;
    } // end while:

    *retbase = (void*) vaddr_range + base_offset;

    return SYS_ERR_OK;
} // end function: elf_allocate

static errval_t elfload_allocate(void *state, genvaddr_t base,
                                 size_t size, uint32_t flags,
                                 void **retbase)
{
    struct monitor_allocate_state *s = state;

    *retbase = (char *)s->vbase + base - s->elfbase;
    return SYS_ERR_OK;
}


/**
 * \brief loads elf image, then relocates it
 * 
 * \param blob_start    
 * \param blob_size
 * \param to
 * \param reloc_dest
 * \param reloc_entry
 */
errval_t 
elf_load_and_relocate(lvaddr_t blob_start, size_t blob_size,
                      void *to, lvaddr_t reloc_dest,
                      uintptr_t *reloc_entry)
{
    genvaddr_t entry; // entry point of the loaded elf image
    struct Elf32_Ehdr *head = (struct Elf32_Ehdr *)blob_start;
    struct Elf32_Shdr *symhead, *rel, *symtab;
    errval_t err;

    //state.vbase = (void *)ROUND_UP(to, ARM_L1_ALIGN);
    struct monitor_allocate_state state;
    state.vbase   = to;
    state.elfbase = elf_virtual_base(blob_start);
    debug_printf("hi 1\n");
    err = elf_load(head->e_machine,
                   elfload_allocate,
                   &state,
                   blob_start, blob_size,
                   &entry);
    if (err_is_fail(err)) {
        return err;
    }
    debug_printf("hi 2\n");
    // Relocate to new physical base address
    symhead = (struct Elf32_Shdr *)(blob_start + (uintptr_t)head->e_shoff);
    rel = elf32_find_section_header_type(symhead, head->e_shnum, SHT_REL);
    symtab = elf32_find_section_header_type(symhead, head->e_shnum, SHT_DYNSYM);
    assert(rel != NULL && symtab != NULL);

    elf32_relocate(reloc_dest, state.elfbase,
                   (struct Elf32_Rel *)(blob_start + rel->sh_offset),
                   rel->sh_size,
                   (struct Elf32_Sym *)(blob_start + symtab->sh_offset),
                   symtab->sh_size,
                   state.elfbase, state.vbase);
    debug_printf("hi 3\n");
    *reloc_entry = entry - state.elfbase + reloc_dest;
    return SYS_ERR_OK;
}

/**
 * \brief Load the elf image
 */
errval_t spawn_arch_load(struct spawninfo *si,
                         lvaddr_t binary, size_t binary_size,
                         genvaddr_t *entry, void** arch_info)
{
    errval_t err;

    // Reset the elfloader_slot
    si->elfload_slot = 0;
    struct capref cnode_cap = {
        .cnode = si->rootcn,
        .slot  = ROOTCN_SLOT_SEGCN,
    };
    err = cnode_create_raw(cnode_cap, &si->segcn, DEFAULT_CNODE_SLOTS, NULL);
    if (err_is_fail(err)) {
        return err_push(err, SPAWN_ERR_CREATE_SEGCN);
    }

    // TLS is NYI
    si->tls_init_base = 0;
    si->tls_init_len = si->tls_total_len = 0;

    debug_printf("spawn_arch_load: about to load elf %p\n", elf_allocate);
    // Load the binary
    err = elf_load(EM_HOST, elf_allocate, si, binary, binary_size, entry);
    if (err_is_fail(err)) {
        return err;
    }

    debug_printf("hello here\n");
    struct Elf32_Shdr* got_shdr =
        elf32_find_section_header_name(binary, binary_size, ".got");
    if (got_shdr)
    {
        *arch_info = (void*)got_shdr->sh_addr;
    }
    else {
        return SPAWN_ERR_LOAD;
    }

    return SYS_ERR_OK;
}

void spawn_arch_set_registers(void *arch_load_info,
                              dispatcher_handle_t handle,
                              arch_registers_state_t *enabled_area,
                              arch_registers_state_t *disabled_area)
{
    assert(arch_load_info != NULL);
    uintptr_t got_base = (uintptr_t)arch_load_info;

    struct dispatcher_shared_arm* disp_arm = get_dispatcher_shared_arm(handle);
    disp_arm->got_base = got_base;

    enabled_area->regs[REG_OFFSET(PIC_REGISTER)] = got_base;
    enabled_area->named.cpsr = CPSR_F_MASK | ARM_MODE_USR;

    disabled_area->regs[REG_OFFSET(PIC_REGISTER)] = got_base;
    disabled_area->named.cpsr = CPSR_F_MASK | ARM_MODE_USR;
}
