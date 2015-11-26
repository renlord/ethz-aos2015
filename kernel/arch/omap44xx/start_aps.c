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
    // TODO: you might want to implement this function
	volatile uint32_t *aux_core_boot_0 = (uint32_t *) AUX_CORE_BOOT_0;
	volatile uint32_t *aux_core_boot_1 = (uint32_t *) AUX_CORE_BOOT_1;

	*(aux_core_boot_0) |= core_id << 2;
	*(aux_core_boot_1) = entry;
	send_event();

    printk(LOG_NOTE, "start_aps_arm_start \n");
#if 0
    printk(LOG_NOTE, "holding core boot core\n");
    while(true);
#endif
    return SYS_ERR_OK;
}
