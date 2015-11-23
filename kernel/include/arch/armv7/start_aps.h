/**
 * \file
 * \brief Definitions for the startup of application processors.
 *
 *  This file contains the prototypes for the functions which start
 *  the application processors
 */

/*
 * Copyright (c) 2007, 2008, 2010, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#ifndef START_APS_H_
#define START_APS_H_

#define AP_STARTING_UP 4422
#define AP_STARTED     6633

#define AP_WAIT_PHYS	((lpaddr_t)0x80020000)
#define AP_GLOBAL_PHYS	((lpaddr_t)0x80021000)


// for PandaBoard
#define AUX_CORE_BOOT_0 ((lpaddr_t)0x48281800)
#define AUX_CORE_BOOT_1 ((lpaddr_t)0x48281804)


// Entry point into the kernel for second core
void app_core_start(void); // defined in boot.S

/**
 * Send event to other core
 * PandaBoard specific
*/

void send_event(void);

int start_aps_arm_start(uint8_t core_id, lvaddr_t entry);

#endif // START_APS_H_
