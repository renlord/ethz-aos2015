/**
 * \file
 * \brief
 */

/*
 * Copyright (c) 2008, 2009, 2010, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#ifndef ARCH_ARM_BARRELFISH_CORESTATE_H
#define ARCH_ARM_BARRELFISH_CORESTATE_H

#include <barrelfish/core_state.h>

struct pinned_state {
    struct thread_mutex mutex;
    lvaddr_t offset;
    struct slab_alloc vregion_list_slab;
    struct slab_alloc frame_list_slab;
};

struct core_state_arch {
    struct core_state_generic c;
    struct pinned_state pinned_state;
};

#endif
