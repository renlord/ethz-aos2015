/*
 * Copyright (c) 2012 ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#include <kernel.h>
#include <barrelfish_kpi/paging_arm_v7.h>
#include <elf/elf.h>
#include <kernel_multiboot.h>
#include <paging_kernel_arch.h>
#include <startup_helpers.h>
#include <startup_arch.h>
#include <barrelfish_kpi/init.h>
#include <barrelfish_kpi/dispatcher_shared.h>
#include <dispatch.h>
#include <string.h>

static inline uintptr_t round_down(uintptr_t value, size_t unit)
{
    assert(0 == (unit & (unit - 1)));
    size_t m = unit - 1;
    return value & ~m;
}

static inline uintptr_t round_up(uintptr_t value, size_t unit)
{
    assert(0 == (unit & (unit - 1)));
    size_t m = unit - 1;
    return (value + m) & ~m;
}

/**
 * The address from where alloc_phys will start allocating memory
 */
lpaddr_t init_alloc_addr = PHYS_MEMORY_START;

/**
 * \brief Linear physical memory allocator.
 *
 * This function allocates a linear region of addresses of size 'size' from
 * physical memory.
 *
 * \param size  Number of bytes to allocate.
 *
 * \return Base physical address of memory region.
 */
lpaddr_t alloc_phys(size_t size)
{
    // round to base page size
    uint32_t npages = (size + BASE_PAGE_SIZE - 1) / BASE_PAGE_SIZE;

    assert(init_alloc_addr != 0);

    lpaddr_t addr = init_alloc_addr;

    init_alloc_addr += npages * BASE_PAGE_SIZE;
    return addr;
}

lpaddr_t alloc_phys_aligned(size_t size, size_t align)
{
	init_alloc_addr = round_up(init_alloc_addr, align);
	return alloc_phys(size);
}

static uint32_t elf_to_l2_flags(uint32_t eflags)
{
    switch (eflags & (PF_W|PF_R))
    {
      case PF_W|PF_R:
        return (ARM_L2_SMALL_USR_RW |
                ARM_L2_SMALL_CACHEABLE |
                ARM_L2_SMALL_BUFFERABLE);
      case PF_R:
        return (ARM_L2_SMALL_USR_RO |
                ARM_L2_SMALL_CACHEABLE |
                ARM_L2_SMALL_BUFFERABLE);
      default:
        panic("Unknown ELF flags combination.");
    }
}

/**
 * Map frames into init process address space. Init has a contiguous set of
 * l2 entries so this is straightforward.
 *
 * @param l2_table      pointer to init's L2 table.
 * @param l2_base       virtual address represented by first L2 table entry
 * @param va_base       virtual address to map.
 * @param pa_base       physical address to associate with virtual address.
 * @param bytes         number of bytes to map.
 * @param l2_flags      ARM L2 small page flags for mapped pages.
 */
static void
spawn_init_map(union arm_l2_entry* l2_table,
               lvaddr_t   l2_base,
               lvaddr_t   va_base,
               lpaddr_t   pa_base,
               size_t     bytes,
               uintptr_t  l2_flags)
{
    assert(va_base >= l2_base);
    assert(0 == (va_base & (BASE_PAGE_SIZE - 1)));
    assert(0 == (pa_base & (BASE_PAGE_SIZE - 1)));
    assert(0 == (bytes & (BASE_PAGE_SIZE - 1)));

    int bi = (va_base - l2_base) / BASE_PAGE_SIZE;
    int li = bi + bytes / BASE_PAGE_SIZE;

    while (bi < li)
    {
        paging_set_l2_entry((uintptr_t *)&l2_table[bi], pa_base, l2_flags);
        pa_base += BASE_PAGE_SIZE;
        bi++;
    }
}

static errval_t
startup_alloc_init(
    void*      state,
    genvaddr_t gvbase,
    size_t     bytes,
    uint32_t   flags,
    void**     ret
    )
{
    const struct startup_l2_info* s2i = (const struct startup_l2_info*)state;

    lvaddr_t sv = round_down((lvaddr_t)gvbase, BASE_PAGE_SIZE);
    size_t   off = (lvaddr_t)gvbase - sv;
    lvaddr_t lv = round_up((lvaddr_t)gvbase + bytes, BASE_PAGE_SIZE);
    lpaddr_t pa;

    //STARTUP_PROGRESS();
    pa = alloc_phys_aligned((lv - sv), BASE_PAGE_SIZE);

    if (lv > sv && (pa != 0))
    {
        spawn_init_map(s2i->l2_table, s2i->l2_base, sv,
                       pa, lv - sv, elf_to_l2_flags(flags));
        *ret = (void*)(local_phys_to_mem(pa) + off);
    }
    else
    {
        *ret = 0;
    }
    return SYS_ERR_OK;
}

static void
load_init_image(
    struct startup_l2_info* l2i,
    const char *name,
    genvaddr_t* init_ep,
    genvaddr_t* got_base
    )
{
    lvaddr_t elf_base;
    size_t elf_bytes;
    errval_t err;


    *init_ep = *got_base = 0;

    /* Load init ELF32 binary */
    struct multiboot_modinfo *module = multiboot_find_module(name);
    if (module == NULL) {
    	panic("Could not find init module!");
    }

    elf_base =  local_phys_to_mem(module->mod_start);
    elf_bytes = MULTIBOOT_MODULE_SIZE(*module);

    printk(LOG_DEBUG, "load_init_image %p %08x\n", elf_base, elf_bytes);

    err = elf_load(EM_ARM, startup_alloc_init, l2i,
    		elf_base, elf_bytes, init_ep);
    if (err_is_fail(err)) {
    	//err_print_calltrace(err);
    	panic("ELF load of %s failed!\n", name);
    }

    // TODO: Fix application linkage so that it's non-PIC.
    struct Elf32_Shdr* got_shdr =
        elf32_find_section_header_name((lvaddr_t)elf_base, elf_bytes, ".got");
    if (got_shdr)
    {
        *got_base = got_shdr->sh_addr;
    }
}


/// Pointer to bootinfo structure for init
struct bootinfo* bootinfo = (struct bootinfo*)INIT_BOOTINFO_VBASE;
static struct spawn_state spawn_state;
static lpaddr_t bootinfo_phys;

struct dcb *allocate_init_dcb(const char *name)
{
    /* Allocate bootinfo */
    bootinfo_phys = alloc_phys(BOOTINFO_SIZE);
    memset((void *)local_phys_to_mem(bootinfo_phys), 0, BOOTINFO_SIZE);

    /* Construct cmdline args */
    char bootinfochar[16];
    snprintf(bootinfochar, sizeof(bootinfochar), "%u", INIT_BOOTINFO_VBASE);
    const char *argv[] = { "init", bootinfochar };
    int argc = 2;

    lvaddr_t paramaddr;
    // allocate and initalize a control block for Init
    return allocate_dcb(&spawn_state, name, argc, argv,
                        bootinfo_phys, INIT_ARGS_VBASE,
                        alloc_phys, &paramaddr);
}

void map_and_load_init(const char *name, struct dcb *init_dcb,
                       union arm_l2_entry *init_l2, genvaddr_t *ret_init_ep,
                       genvaddr_t *ret_got_base)
{
    printf("mapping arguments page\n");
    // Map arguments page
    spawn_init_map(init_l2, INIT_VBASE, INIT_ARGS_VBASE,
                   spawn_state.args_page, ARGS_SIZE, INIT_PERM_RW);

    // Map dispatcher
    printf("mapping dispatcher\n");
    spawn_init_map(init_l2, INIT_VBASE, INIT_DISPATCHER_VBASE,
                   mem_to_local_phys(init_dcb->disp), DISPATCHER_SIZE,
                   INIT_PERM_RW);

    // Map bootinfo
    printf("mapping bootinfo\n");
    spawn_init_map(init_l2, INIT_VBASE, INIT_BOOTINFO_VBASE,
                   bootinfo_phys, BOOTINFO_SIZE, INIT_PERM_RW);

    struct startup_l2_info l2_info = { init_l2, INIT_VBASE };

    printf("calling load_init_image\n");
    load_init_image(&l2_info, name, ret_init_ep, ret_got_base);

    printf("got %"PRIxGENVADDR" as init ep from load_init_image\n", *ret_init_ep);

    printf("bootinfo: 0x%08x\n", bootinfo);
    printf("init_l2[2]: 0x%08x\n", init_l2[2]);
    /* Fill bootinfo struct */
    bootinfo->mem_spawn_core = KERNEL_IMAGE_SIZE; // Size of kernel

    
    /* Initialize name field in generic part */
    struct dispatcher_shared_generic *disp
        = get_dispatcher_shared_generic(init_dcb->disp);
    strncpy(disp->name, "init", DISP_NAME_LEN);

}
