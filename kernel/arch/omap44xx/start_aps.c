/**
 * \file
 * \brief Start the application processors
 *
 *  This file sends all needed IPIs to the other cores to start them.
 */

/*
 * Copyright (c) 2007, 2008, 2010, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#include <kernel.h>
#include <stdio.h>
#include <string.h>
#include <paging_kernel_arch.h>
#include <arch/armv7/start_aps.h>
#include <arm_hal.h>
#include <cp15.h>

#define STARTUP_TIMEOUT         0xffffff

#define AUX_CORE_BOOT_SECT      (AUX_CORE_BOOT_0 & ~ARM_L1_SECTION_MASK)
#define AUX_CORE_BOOT_0_OFFSET  (AUX_CORE_BOOT_0 & ARM_L1_SECTION_MASK)
#define AUX_CORE_BOOT_1_OFFSET  (AUX_CORE_BOOT_1 & ARM_L1_SECTION_MASK)

/**
 * Send event to other core
 * Note: this is PandaBoard_specific
 */
void send_event(void)
{
    __asm__ volatile ("SEV");
}



// // Entry point into the kernel for second core
// void app_core_start(void); // defined in boot.S


/**
 * \brief Boot an arm app core
 *
 * \param core_id   APIC ID of the core to try booting
 * \param entry     Entry address for new kernel in the destination
 *                  architecture's lvaddr_t
 *
 * \returns Zero on successful boot, non-zero (error code) on failure
 */
int start_aps_arm_start(uint8_t core_id, lpaddr_t entry)
{
    /* pointer to the pseudo-lock used to detect boot up of new core */
    volatile uint32_t *ap_wait = (uint32_t *) local_phys_to_mem(AP_WAIT_PHYS);
    *ap_wait = AP_STARTING_UP;
    cp15_invalidate_d_cache();

    // map AUX_CORE_BOOT section
    static lvaddr_t aux_core_boot = 0;
    if (aux_core_boot == 0) {
        aux_core_boot = paging_map_device(AUX_CORE_BOOT_SECT, 
                                          ARM_L1_SECTION_BYTES);
    }
    volatile lvaddr_t *aux_core_boot_0, *aux_core_boot_1;
    aux_core_boot_0 = (void *)(aux_core_boot + AUX_CORE_BOOT_0_OFFSET);
    aux_core_boot_1 = (void *)(aux_core_boot + AUX_CORE_BOOT_1_OFFSET);

    *aux_core_boot_1 = entry;

    *aux_core_boot_0 |= 1 << 2;

    printk(LOG_NOTE, "start_aps_arm_start \n");
    send_event();
    while(*aux_core_boot_0 != (2 << 2));
    printk(LOG_NOTE, "booted CPU%hhu\n", core_id);
#if 0
    printk(LOG_NOTE, "holding core boot core\n");
    while(true);
#endif
    return SYS_ERR_OK;
}
