/**
 * \file
 * \brief Startup prototypes.
 */

/*
 * Copyright (c) 2007, 2008, 2009, 2010, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#ifndef __KERNEL_STARTUP_H
#define __KERNEL_STARTUP_H

struct spawn_state {
    /// Address of arguments page
    lpaddr_t args_page;
};

/**
 * \brief Linear physical memory allocator callback function.
 *
 * This function allocates a linear region of addresses of size 'size' from
 * physical memory.
 *
 * \param size  Number of bytes to allocate.
 *
 * \return Base physical address of memory region.
 */
typedef lpaddr_t (*alloc_phys_func)(size_t size);

struct dcb *allocate_dcb(struct spawn_state *st,
                         const char *name, int argc, const char** argv,
                         lpaddr_t bootinfo, lvaddr_t args_base,
                         alloc_phys_func alloc_phys, lvaddr_t *retparamaddr);

#endif // __KERNEL_STARTUP_H
