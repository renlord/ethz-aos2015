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
#include <barrelfish/barrelfish.h>
#include <spawndomain/spawndomain.h>
#include <stdlib.h>
#include <string.h>
#include <barrelfish/morecore.h>
#include <barrelfish/dispatcher_arch.h>
#include <barrelfish/debug.h>
#include <barrelfish/lmp_chan.h>
#include <barrelfish/aos_rpc.h>
#include <barrelfish/sys_debug.h>
#include <barrelfish/sys_debug.h>
#include <omap44xx_map.h>

// #include "../spawnd/spawnd.h"

extern struct bootinfo *bi;

errval_t initialize_ram_alloc(void);
errval_t initialize_mem_serv(void);

errval_t memserv_alloc(struct capref *ret, uint8_t bits,
                       genpaddr_t minbase, genpaddr_t maxlimit);

errval_t serial_put_char(const char *c);
errval_t serial_get_char(char *c);
void my_print(const char *buf);
void my_read(void);

void recv_handler(void *lc_in);
void send_handler(void *client_state_in);

void set_uart3_registers(lvaddr_t base);
errval_t spawn_spawnd(void);

#endif // INIT_H
