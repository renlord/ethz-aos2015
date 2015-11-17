/**
 * \file
 * \brief
 */

/*
 * Copyright (c) 2007, 2008, 2009, 2010, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <barrelfish/barrelfish.h>
#include <spawndomain/spawndomain.h>
#include "spawn.h"

#ifndef min
#define min(a,b) ((a) < (b) ? (a) : (b))
#endif
#ifndef max
#define max(a,b) ((a) > (b) ? (a) : (b))
#endif

const char *multiboot_strings = NULL;

const char *getopt_module(struct mem_region *module)
{
    assert(module != NULL);
    assert(module->mr_type == RegionType_Module);

    const char *optstring = multiboot_strings + module->mrmod_data;

    // find the first space (or end of string if there is none)
    const char *args = strchr(optstring, ' ');
    if (args == NULL) {
        args = optstring + strlen(optstring);
    }

    // search backward for last '/' before the first ' '
    for (const char *c = args; c > optstring; c--) {
        if (*c == '/') {
            return c + 1;
        }
    }

    return optstring;
}

/// Map in the frame caps for a module into our vspace, return their location
errval_t spawn_map_module(struct mem_region *module, size_t *retsize,
                          lvaddr_t *retaddr, genpaddr_t *retpaddr)
{
    assert(module != NULL);
    assert(module->mr_type == RegionType_Module);

    errval_t err;

    // Find and round up the size of module, and save it back in retsize
//    debug_printf("%s %d\n", __FILE__, __LINE__);
    size_t size = module->mrmod_size;
    size = ROUND_UP(size, BASE_PAGE_SIZE);
    if (retsize) {
        *retsize = size;
    }

    // Get virtual address range to hold the module
//    debug_printf("%s %d\n", __FILE__, __LINE__);
    void *vaddr_range;
    err = paging_alloc(get_current_paging_state(), &vaddr_range, size);
    if (err_is_fail(err)) {
        printf("spawn_map_module: paging_alloc failed\n");
        return (err);
    }

    // start_addr, rounded_up_size,

    struct capref frame = {
        .cnode = cnode_module,
        .slot  = module->mrmod_slot,
    };

    if (retpaddr != NULL) {
        *retpaddr = module->mr_base;
    }

    if (retsize != NULL) {
        *retsize = size;
    }

    if (retaddr != NULL) {
        *retaddr = (lvaddr_t)vaddr_range;
    }

    lvaddr_t vaddr = (lvaddr_t)vaddr_range;

    while (size > 0) {
//        debug_printf("%s %d %zu\n", __FILE__, __LINE__, size);
        assert((size & BASE_PAGE_MASK) == 0);

        struct frame_identity id;
//        uint8_t invoke_bits = get_cap_valid_bits(frame);
//        capaddr_t invoke_cptr = get_cap_addr(frame) >> (CPTR_BITS - invoke_bits);
//        debug_printf("icptr = 0x%"PRIxCADDR", bits = %"PRIu8"\n",
//                invoke_cptr, invoke_bits);

        err = invoke_frame_identify(frame, &id);
        if (err_is_fail(err)) {
            DEBUG_ERR(err, "frame identify in spawn_map_module");
            return err;
        }
        assert(err_is_ok(err));

        // map frame to provide physical memory backing
        err = paging_map_fixed_attr(get_current_paging_state(), vaddr, frame,
                (1UL << id.bits), VREGION_FLAGS_READ);
                // VREGION_FLAGS_READ_EXECUTE);
                // FIXME: VREGION_FLAGS_READ_WRITE);
        if (err_is_fail(err)) {
            debug_printf("spawn_map_module: paging_map_fixed_attr failed\n");
            return err;
        }

        frame.slot ++;
        size -= (1UL << id.bits);
        vaddr += (1UL << id.bits);
    }

    return SYS_ERR_OK;
}

errval_t spawn_unmap_module(lvaddr_t mapped_addr)
{
    return paging_unmap(get_current_paging_state(), (void *)mapped_addr);
}

/// Returns a raw pointer to the modules string area string
const char *multiboot_module_rawstring(struct mem_region *region)
{
    if (multiboot_strings == NULL) {
	errval_t err;
        /* Map in multiboot module strings area */
        struct capref mmstrings_cap = {
            .cnode = cnode_module,
            .slot = 0
        };
        err = paging_map_frame_attr(get_current_paging_state(),
                                    (void**)&multiboot_strings,
                                    BASE_PAGE_SIZE, mmstrings_cap,
                                    VREGION_FLAGS_READ,
                                    NULL, NULL);
        if (err_is_fail(err)) {
            DEBUG_ERR(err, "paging_map failed");
	    return NULL;
        }
#if 1
        printf("Mapped multiboot_strings at %p\n", multiboot_strings);
        for (int i = 0; i < 256; i++) {
            if ((i & 15) == 0) printf("%04x  ", i);
            printf ("%02x ", multiboot_strings[i]& 0xff);
            if ((i & 15) == 15) printf("\n");
        }
#endif
    }

    if (region == NULL || region->mr_type != RegionType_Module) {
        return NULL;
    }
    return multiboot_strings + region->mrmod_data;
}

errval_t multiboot_cleanup_mapping(void)
{
    errval_t err = paging_unmap(get_current_paging_state(), multiboot_strings);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "multiboot_cleanup_mapping: paging_unmap() failed\n");
        return err_push(err, LIB_ERR_VSPACE_REMOVE_REGION);
    }
    multiboot_strings = NULL;
    return SYS_ERR_OK;
}


/// returns the basename without arguments of a multiboot module
// XXX: returns pointer to static buffer. NOT THREAD SAFE
const char *multiboot_module_name(struct mem_region *region)
{
    const char *str = multiboot_module_rawstring(region);
    if (str == NULL) {
	return NULL;
    }

    // copy module data to local buffer so we can mess with it
    static char buf[128];
    strncpy(buf, str, sizeof(buf));
    buf[sizeof(buf) - 1] = '\0'; // ensure termination

    // ignore arguments for name comparison
    char *args = strchr(buf, ' ');
    if (args != NULL) {
        *args = '\0';
    }

    return buf;
}

struct mem_region *multiboot_find_module(struct bootinfo *bi, const char *name)
{
    for(size_t i = 0; i < bi->regions_length; i++) {
        struct mem_region *region = &bi->regions[i];

        const char *modname = multiboot_module_name(region);
        if (modname != NULL &&
            strncmp(modname + strlen(modname) - strlen(name),
                    name, strlen(name)) == 0) {
            return region;
        }
    }

    return NULL;
}

struct mem_region *multiboot_find_module_containing(struct bootinfo *bi,
						    const char *name,
						    const char *containing)
{
    assert(bi != NULL);
    assert(name != NULL);
    assert(containing != NULL);

    size_t namelen = strlen(name);

    for(size_t i = 0; i < bi->regions_length; i++) {
        struct mem_region *region = &bi->regions[i];
	if (region->mr_type != RegionType_Module)
	    continue;
	const char *raw = multiboot_module_rawstring(region);
	if (raw == NULL)
	    continue;

	const char *space = strchr(raw, ' ');
	if (space == NULL || (space - raw < namelen))
	    continue;

	if (strncmp(space - namelen, name, namelen))
	    continue;

	if ((strstr(raw, containing)) != NULL)
	    return region;
    }

    return NULL;
}
