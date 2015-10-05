/**
 * \file
 * \brief MMCHS Driver main routine.
 */
/*
 * Copyright (c) 2013, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#include <stdlib.h>
#include <barrelfish/aos_rpc.h>
#include <arch/arm/omap44xx/device_registers.h>

#include "mmchs.h"

static struct cnoderef cnode;

static void get_cap(lpaddr_t base, size_t size)
{
    errval_t err;
    size_t len;

    struct capref cap;
    err = aos_rpc_get_dev_cap(get_init_rpc(),
            base, size, &cap, &len);

    if (err_is_fail(err)) {
        USER_PANIC_ERR(err, "get_dev_mem rpc failed\n");
    }

    struct capref argcn = {
        .cnode = cnode_root,
        .slot = TASKCN_SLOT_ARGSPAGE
    };

    size_t bits = 8; // TODO(gz): How do I figure this value out on the fly?
    struct capref device_cap_iter = {
        .cnode = build_cnoderef(argcn, bits),
        .slot = 0
    };

    static size_t current_slot = 0;
    device_cap_iter.slot = current_slot++;

    err = cap_copy(device_cap_iter, cap);
    if (err_is_fail(err)) {
        USER_PANIC_ERR(err, "cap_copy failed.");
    }
}

int main(int argc, char **argv)
{
    //
    // Initializing the SD-Card
    //

    // Getting the necessary capabilities from init
    struct capref argcn = {
        .cnode = cnode_root,
        .slot = TASKCN_SLOT_ARGSPAGE
    };

    errval_t err = cnode_create_raw(argcn, &cnode, 255, NULL);
    if (err_is_fail(err)) {
        USER_PANIC_ERR(err, "can not create the cnode failed.");
    }

    get_cap(OMAP44XX_CM2 & ~0xFFF, 0x1000);
    get_cap(OMAP44XX_CLKGEN_CM2 & ~0xFFF, 0x1000);
    get_cap(OMAP44XX_I2C1 & ~0xFFF, 0x1000);
    get_cap(OMAP44XX_SYSCTRL_PADCONF_CORE & ~0xFFF, 0x1000);
    get_cap(OMAP44XX_MMCHS1 & ~0xFFF, 0x1000);

    // Initializing the necessary subsystem and the mmchs controlelr
    cm2_init();
    ti_twl6030_init();
    ctrlmod_init();
    cm2_enable_hsmmc1();
    sdmmc1_enable_power();

    mmchs_init();


    //
    // Reading the first block from the SD-Card
    // This should be the same information you get with
    // dd when reading the first block on linux...
    //

    void *buffer = malloc(512);
    assert(buffer != NULL);

    err = mmchs_read_block(0, buffer);
    assert(err_is_ok(err));
    printf("Read block %d:\n", 0);
    for (int i = 1; i <= 512; ++i)
    {
        printf("%"PRIu8"\t", ((uint8_t*) buffer)[i-1]);
        if (i % 4 == 0) {
            printf("\n");
        }
    }

    return 0;
}