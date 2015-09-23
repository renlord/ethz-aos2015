/**
 * \file
 * \brief Kernel serial driver for the OMAP44xx UARTs.  Implements the
 * interface found in /kernel/include/serial.h
 */

/*
 * Copyright (c) 2012, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#include <kernel.h>
#include <arm.h>
#include <paging_kernel_arch.h>

#include <serial.h>

#define UART_BASE 0x48020000
#define UART_SIZE 0x1000

static volatile uint32_t *uart_thr = (uint32_t *)(UART_BASE + 0x0000);
static volatile uint32_t *uart_ier = (uint32_t *)(UART_BASE + 0x0004);
static volatile uint32_t *uart_fcr = (uint32_t *)(UART_BASE + 0x0008);
static volatile uint32_t *uart_lcr = (uint32_t *)(UART_BASE + 0x000C);
static volatile uint32_t *uart_lsr = (uint32_t *)(UART_BASE + 0x0014);

static void set_register_addresses(lvaddr_t base)
{
    uart_thr = (uint32_t *)(base + 0x0000);
    uart_ier = (uint32_t *)(base + 0x0004);
    uart_fcr = (uint32_t *)(base + 0x0008);
    uart_lcr = (uint32_t *)(base + 0x000C);
    uart_lsr = (uint32_t *)(base + 0x0014);
}

static bool uart_initialized = false;
/**
 * \brief Reinitialize console UART after setting up the MMU
 * This function is needed in milestone 1.
 */
void serial_map_registers(void)
{
    if (uart_initialized) {
        printf("omap serial map registers: serial registers already mapped, skipping\n");
        return;
    }

    lvaddr_t base = paging_map_device(UART_BASE, UART_SIZE);
    // paging_map_device returns an address pointing to the beginning of
    // a section, need to add the offset for within the section again
    uint32_t offset = (UART_BASE & ARM_L1_SECTION_MASK);
    printf("omap serial_map_registers: base = 0x%"PRIxLVADDR" 0x%"PRIxLVADDR"\n",
	   base, base + offset);
    set_register_addresses(base + offset);
    uart_initialized = true;
    printf("omap serial_map_registers: done.\n");
    return;
}

/**
 * \brief Initialize the console UART (no mapping)
 */
void serial_init(void)
{
    set_register_addresses(UART_BASE);
    // Disable all interrupts
    *uart_ier = 0;

    // Enable FIFOs and select highest trigger levels
    // i.e.  RX_FIFO_TRIG = TX_FIFO_TRIG = 3, FIFO_EN = 1
    *uart_fcr = 0x000000f1;

    // Set line to 8 bits, no parity, 1 stop bit
    *uart_lcr = 0x00000003;
}

/**
 * \brief Prints a single character to a serial port. 
 */
void serial_putchar(char c)
{
    // we need to send \r\n over the serial line for a newline
    if (c == '\n') serial_putchar('\r');

    // TODO: Wait until FIFO can hold more characters (i.e. TX_FIFO_E == 1)

    // TODO: Write character
}
