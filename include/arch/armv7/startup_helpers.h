/*
 * Copyright (c) 2012 ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#ifndef STARTUP_HELPERS_H
#define STARTUP_HELPERS_H

#include <kernel.h>
#include <paging_kernel_arch.h>

struct startup_l2_info
{
    union arm_l2_entry* l2_table;
    lvaddr_t   l2_base;
};

// address from which alloc_phys allocated memory
extern lpaddr_t init_alloc_addr;
// linear memory allocator
lpaddr_t alloc_phys(size_t size);
lpaddr_t alloc_phys_aligned(size_t size, size_t align);

struct dcb *allocate_init_dcb(const char *name);

void map_and_load_init(const char *name, struct dcb *init_dcb,
                       union arm_l2_entry *init_l2, genvaddr_t *ret_init_ep,
                       genvaddr_t *ret_got_base);

#endif //STARTUP_HELPERS_H
