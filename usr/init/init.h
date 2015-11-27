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

extern struct bootinfo *bi;

enum ps_status {
	PS_STATUS_RUNNING,
	PS_STATUS_ZOMBIE,
	PS_STATUS_SLEEP
};

struct ps_entry {
	domainid_t pid;
	enum ps_status status;
	char *argv[MAX_CMDLINE_ARGS];
	char *argbuf;
	size_t argbytes;
	struct capref rootcn_cap, dcb;
	struct cnoderef rootcn;
	uint8_t exitcode;
};

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

#endif // INIT_H
