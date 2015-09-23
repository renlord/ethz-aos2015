/**
 * \file
 * \brief Architecture-independent interface to the kernel serial port
 * subsystem.  
 */
/*
 * Copyright (c) 2007-2012 ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, CAB F.78, Universitaestr. 6, CH-8092 Zurich. 
 * Attn: Systems Group.
 */

#ifndef __SERIAL_H
#define __SERIAL_H

/*
 * What kind of serial ports do we have?
 */
extern const unsigned serial_num_physical_ports;

/*
 * Initialize a physical serial port
 */
extern void serial_init(void);
extern void serial_map_registers(void);

/*
 * Polled, blocking input/output.  No buffering.
 */
extern void serial_putchar( char c);
extern char serial_getchar( void );

#endif //__SERIAL_H
