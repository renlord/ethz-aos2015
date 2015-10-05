/**
 * \file lib/omap_timer/timer.c
 * \brief omap global timer
 */

/*
 * Copyright (c) 2013, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Universitaetstr. 6, CH-8092 Zurich. Attn: Systems Group.
 */

#include <barrelfish/barrelfish.h>
#include <barrelfish/aos_rpc.h>
#include <omap_timer/timer.h>

#define REGION_OFFSET 0x200

#define OMAP_TIMER_CTRL(addr) ((void *)((char*)(addr) + 0x08))
#define OMAP_TIMER_LOW(addr)  ((void *)((char*)(addr) + 0x00))
#define OMAP_TIMER_HIGH(addr) ((void *)((char*)(addr) + 0x04))

//XXX: should do this more general!
static void *priv_mem = NULL;
static volatile uint32_t *timer_mem = NULL;

static inline uint32_t cp15_read_cbar(void)
{
  uint32_t cbar;
  __asm volatile ("mrc p15, 4, %[cbar], c15, c0, 0" : [cbar] "=r" (cbar));
  return cbar & ~0x1FFF; // Only [31:13] is valid
}

errval_t omap_timer_init(void)
{
    if (priv_mem) return SYS_ERR_OK;

    errval_t err;

    struct capref pmcap;
    size_t retsize;
    err = aos_rpc_get_dev_cap(get_init_rpc(), cp15_read_cbar(), 8092, &pmcap, &retsize);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "omap_timer_init");
        return err;
    }

    // map private region uncached
    err = paging_map_frame_attr(get_current_paging_state(), &priv_mem, retsize,
            pmcap, VREGION_FLAGS_READ_WRITE_NOCACHE, NULL, NULL);
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "omap_timer_init");
        return err;
    }

    timer_mem = (uint32_t *)((char*)priv_mem + REGION_OFFSET);

    volatile uint32_t *ctrl = OMAP_TIMER_CTRL(timer_mem);

    // disable timer, autoreload, and interrupt
    *ctrl = (*ctrl & 0xFFFFFFF4) | 0;

    return SYS_ERR_OK;
}

void omap_timer_ctrl(bool enable)
{
    volatile uint32_t *ctrl = OMAP_TIMER_CTRL(timer_mem);
    // set enable bit to arg value
    *ctrl = (*ctrl & 0xFFFFFFFE) | enable;
}

uint64_t omap_timer_read(void)
{
    volatile uint32_t *low = OMAP_TIMER_LOW(timer_mem);
    volatile uint32_t *high = OMAP_TIMER_HIGH(timer_mem);

    // algo (cortex a9 mpcore trm, page 4-9):
    // 1. read upper 32 bits
    // 2. read lower 32 bits
    // 3. read upper 32 bits again, if u1 != u2 goto 2 else return u << 32 | l

    uint32_t u, l;
    do {
        u = *high;
        l = *low;
    } while (*high != u);

    return ((((uint64_t)u) << 32) | (uint64_t)l);
}
