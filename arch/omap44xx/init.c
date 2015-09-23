/*
 * Copyright (c) 2009-2013, ETH Zurich. All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, CAB F.78, Universitaetstr. 6, CH-8092 Zurich,
 * Attn: Systems Group.
 */

/**
 * \file
 * \brief CPU driver init code for the OMAP44xx series SoCs.
 * interface found in /kernel/include/serial.h
 */

#include <kernel.h>
#include <string.h>
#include <init.h>
#include <exec.h>

#include <serial.h>
#include <stdio.h>
#include <global.h>

#include <arm_hal.h>
#include <arm_core_data.h>
#include <exceptions.h>
#include <kernel_multiboot.h>
#include <paging_kernel_arch.h>
#include <startup_arch.h>

#include <omap44xx_map.h>
#include <omap44xx_led.h>

/**
 * \brief Kernel stack.
 *
 * This is the one and only kernel stack for a kernel instance.
 */
uintptr_t kernel_stack[KERNEL_STACK_SIZE/sizeof(uintptr_t)]
          __attribute__ ((aligned(8)));

// You might want to use this or some other fancy/creative banner ;-)
static const char banner[] =
"   ___                   ______     __ \n"\
"  / _ )___ ____________ / / _(_)__ / / \n"\
" / _  / _ `/ __/ __/ -_) / _/ (_-</ _ \\\n"\
"/____/\\_,_/_/ /_/  \\__/_/_//_/___/_//_/\n";

/**
 * \brief Continue kernel initialization in kernel address space.
 *
 * This function resets paging to map out low memory and map in physical
 * address space, relocating all remaining data structures. It sets up exception handling,
 * initializes devices and enables interrupts. After that it
 * calls arm_kernel_startup(), which should not return (if it does, this function
 * halts the kernel).
 */
static void  __attribute__ ((noinline,noreturn)) text_init(void)
{
    // Map-out low memory
    struct arm_coredata_mmap *mmap = (struct arm_coredata_mmap *)
        local_phys_to_mem(glbl_core_data->mmap_addr);

    printf("paging_arm_reset: base: 0x%"PRIx64", length: 0x%"PRIx64".\n",
           mmap->base_addr, mmap->length);
    paging_arm_reset(mmap->base_addr, mmap->length);

    exceptions_init();

    printf("invalidate cache\n");
    cp15_invalidate_i_and_d_caches_fast();

    printf("invalidate TLB\n");
    cp15_invalidate_tlb_fn();

    // reinitialize serial driver in mapped address space
    serial_map_registers();

    // map led gpio reg
    led_map_register();

    // flash leds again
    led_flash();

    printf("The address of paging_map_kernel_section is %p\n",
           paging_map_kernel_section);

    // Test MMU by remapping the device identifier and reading it using a
    // virtual address
    lpaddr_t id_code_section = OMAP44XX_MAP_L4_CFG_SYSCTRL_GENERAL_CORE & ~ARM_L1_SECTION_MASK;
    lvaddr_t id_code_remapped = paging_map_device(id_code_section,
                                                  ARM_L1_SECTION_BYTES);
    lvaddr_t id = (id_code_remapped + (OMAP44XX_MAP_L4_CFG_SYSCTRL_GENERAL_CORE & ARM_L1_SECTION_MASK));

    printf("original addr: 0x%"PRIxLPADDR"\n", id_code_section);
    printf("remapped addr: 0x%"PRIxLVADDR"\n", id_code_remapped);

    char buf[32] = {0};
    memcpy(buf,(const char *)(id+0x200),32);
    printf("Using MMU (addr 0x%"PRIxLVADDR"):\n", id);
    for (int i = 0; i < 32; i+=8) {
        printf("%03x: ", i);
        printf("%02x ", buf[i]);
        printf("%02x ", buf[i+1]);
        printf("%02x ", buf[i+2]);
        printf("%02x ", buf[i+3]);
        printf("%02x ", buf[i+4]);
        printf("%02x ", buf[i+5]);
        printf("%02x ", buf[i+6]);
        printf("%02x\n", buf[i+7]);
    }
    gic_init();
    printf("gic_init done\n");

    arm_kernel_startup();
    panic("arm_kernel_startup() returned\n");
}

/**
 * \brief extract global information from multiboot_info
 */
static void parse_multiboot_image_header(struct multiboot_info *mb)
{
    // set global data struct
    memset(glbl_core_data, 0, sizeof(struct arm_core_data));
    // figure out first usable memory address:
    // max(&kernel_final_byte, multiboot->end_address)
    glbl_core_data->start_free_ram = ROUND_UP(max(multiboot_end_addr(mb),
                                  (uintptr_t)&kernel_final_byte), BASE_PAGE_SIZE);

    glbl_core_data->mods_addr = mb->mods_addr;
    glbl_core_data->mods_count = mb->mods_count;
    glbl_core_data->cmdline = mb->cmdline;
    glbl_core_data->mmap_length = mb->mmap_length;
    glbl_core_data->mmap_addr = mb->mmap_addr;
    glbl_core_data->multiboot_flags = mb->flags;
}

extern void paging_map_device_section(uintptr_t ttbase, lvaddr_t va, lpaddr_t pa);

/**
 * Create initial (temporary) page tables.
 *
 * TODO: you need to implement this function for milestone 1.
 */
static void paging_init(void)
{
}

/**
 * Entry point called from boot.S for bootstrap processor.
 * if is_bsp == true, then pointer points to multiboot_info
 * else pointer points to a global struct
 */
void arch_init(void *pointer)
{
    serial_init(); // TODO: complete implementation of serial_init
    // You should be able to call serial_purchar(42); here.
    // Also, you can call printf here!
    arch_init(42);
    
    // TODO: Produce some output that will surprise your TA.
    // TODO: complete implementation of led_flash -- implementation is in
    // kernel/arch/omap44xx/omap_led.c
    led_flash();

    for(;;); // Infinite loop to keep the system busy for milestone 0.

    // You will need this section of the code for milestone 1.
    struct multiboot_info *mb = (struct multiboot_info *)pointer;
    parse_multiboot_image_header(mb);

    paging_init();
    cp15_enable_mmu();
    printf("MMU enabled\n");

    text_init();
}
