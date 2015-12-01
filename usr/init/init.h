/**
 * \file
 * \brief
 */

/*
 * Copyright (c) 2007, 2008, 2009, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#ifndef INIT_H
#define INIT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <barrelfish/barrelfish.h>
#include <barrelfish/morecore.h>
#include <barrelfish/dispatcher_arch.h>
#include <barrelfish/debug.h>
#include <barrelfish/lmp_chan.h>
#include <barrelfish/aos_rpc.h>
#include <barrelfish/sys_debug.h>
#include <barrelfish/sys_debug.h>
#include <omap44xx_map.h>
#include <spawndomain/spawndomain.h>

extern struct bootinfo *bi;

static volatile uint32_t *uart3_thr = NULL;
static volatile uint32_t *uart3_rhr = NULL;
static volatile uint32_t *uart3_fcr = NULL;
static volatile uint32_t *uart3_lsr = NULL;

enum urpc_request {
	REMOTE_SPAWN,

};

enum urpc_response {
	REMOTE_SPAWN_DONE,
	REMOTE_SPAWN_FAIL,
};

errval_t initialize_ram_alloc(void);
errval_t initialize_mem_serv(void);

errval_t memserv_alloc(struct capref *ret, uint8_t bits,
                       genpaddr_t minbase, genpaddr_t maxlimit);

static inline void set_uart3_registers(lvaddr_t base)
{
    uart3_thr = (uint32_t *)((uint32_t)base + 0x0000);
    uart3_rhr = (uint32_t *)((uint32_t)base + 0x0000);
    uart3_fcr = (uint32_t *)((uint32_t)base + 0x0008);
    uart3_lsr = (uint32_t *)((uint32_t)base + 0x0014);
}

static inline errval_t serial_put_char(const char *c) 
{
    //debug_printf("serial put char ----> %c\n\n", *c);
    assert(uart3_lsr && uart3_thr);
    while(*uart3_lsr == 0x20);
    *uart3_thr = *c;

    return SYS_ERR_OK;
}

static inline errval_t serial_get_char(char *c) 
{
    *uart3_fcr &= ~1; // write 0 to bit 0
    *uart3_rhr = 0;
    while((*uart3_lsr & 1) == 0);
    *c = (char)*uart3_rhr;

    return SYS_ERR_OK;
}

void recv_handler(void *lc_in);
void send_handler(void *client_state_in);

#endif // INIT_H
