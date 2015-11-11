/*
 * Copyright (c) 2009, 2010 ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
 */

#include <kernel.h>
#include <dispatch.h>
#include <string.h>
#include <stdio.h>

#include <barrelfish_kpi/init.h>
#include <barrelfish_kpi/syscalls.h>
#include <elf/elf.h>

#include <arm_hal.h>
#include <paging_kernel_arch.h>
#include <exceptions.h>
#include <cp15.h>
#include <cpiobin.h>
#include <init.h>
#include <barrelfish_kpi/paging_arm_v7.h>
#include <arm_core_data.h>
#include <kernel_multiboot.h>
#include <offsets.h>
#include <startup_arch.h>
#include <global.h>

#define CNODE(cte)              (cte)->cap.u.cnode.cnode
#define C(cte)                  (&(cte)->cap)
#define UNUSED(x)               (x) = (x)

/* Task CNode slots */
#define TASKCN_SLOT_SELFEP      (TASKCN_SLOTS_USER + 0)   ///< Endpoint to self
#define TASKCN_SLOT_INITEP      (TASKCN_SLOTS_USER + 1)   ///< End Point to init
#define TASKCN_SLOT_REMEP       (TASKCN_SLOTS_USER + 2)   ///< End Point to init

// first EP starts in dispatcher frame after dispatcher (33472 bytes)
// and the value we use for the mint operation is the offset of the kernel
// struct in the lmp_endpoint struct (+56 bytes)
// FIRSTEP_OFFSET = get_dispatcher_size() + offsetof(struct lmp_endpoint, k)
#define FIRSTEP_OFFSET          (33472u + 56u)
// buflen is in words for mint
// FIRSTEP_BUFLEN = (sizeof(struct lmp_endpoint) +
//                  (DEFAULT_LMP_BUF_WORDS + 1) * sizeof(uintptr_t))
#define FIRSTEP_BUFLEN          21u

#define STARTUP_PROGRESS()      debug(SUBSYS_STARTUP, "%s:%d\n",          \
                                      __FUNCTION__, __LINE__);

#if MILESTONE == 2
#define BSP_INIT_MODULE_NAME    "armv7/sbin/init_memtest"
#else
#define BSP_INIT_MODULE_NAME    "armv7/sbin/init"
#endif
#define ADDTITIONAL_MODULE_NAME "armv7/sbin/memeater"

static union arm_l1_entry * init_l1;              // L1 page table for init
static union arm_l2_entry * init_l2;              // L2 page tables for init

//static struct spawn_state spawn_state;

/// Pointer to bootinfo structure for init
struct bootinfo* bootinfo = (struct bootinfo*)INIT_BOOTINFO_VBASE;

/**
 * Each kernel has a local copy of global and locks. However, during booting and
 * kernel relocation, these are set to point to global of the pristine kernel,
 * so that all the kernels can share it.
 */
struct global *global = (struct global *)GLOBAL_VBASE;

static inline uintptr_t round_up(uintptr_t value, size_t unit)
{
    assert(0 == (unit & (unit - 1)));
    size_t m = unit - 1;
    return (value + m) & ~m;
}

static inline uintptr_t round_down(uintptr_t value, size_t unit)
{
    assert(0 == (unit & (unit - 1)));
    size_t m = unit - 1;
    return value & ~m;
}

extern size_t ram_size;

static lpaddr_t core_local_alloc_start = PHYS_MEMORY_START;
static lpaddr_t core_local_alloc_end = PHYS_MEMORY_START;

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
static lpaddr_t bsp_alloc_phys(size_t size)
{
    // round to base page size
    uint32_t npages = (size + BASE_PAGE_SIZE - 1) / BASE_PAGE_SIZE;

    assert(core_local_alloc_start != 0);
    assert((core_local_alloc_start + (npages * BASE_PAGE_SIZE))
            < core_local_alloc_end);
    lpaddr_t addr = core_local_alloc_start;

    core_local_alloc_start += npages * BASE_PAGE_SIZE;
    return addr;
}

static lpaddr_t bsp_alloc_phys_aligned(size_t size, size_t align)
{
    core_local_alloc_start = round_up(core_local_alloc_start, align);
    return bsp_alloc_phys(size);
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
    debug(SUBSYS_STARTUP, "spawn_init_map %p %p\n", va_base, l2_base);
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

struct startup_l2_info
{
    union arm_l2_entry* l2_table;
    lvaddr_t   l2_base;
};

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
    pa = bsp_alloc_phys_aligned((lv - sv), BASE_PAGE_SIZE);

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

    debug(SUBSYS_STARTUP, "load_init_image called for %s\n", name);
    /* Load init ELF32 binary */
    struct multiboot_modinfo *module = multiboot_find_module(name);
    if (module == NULL) {
    	panic("Could not find init module! %s\n", name);
    }

    elf_base =  local_phys_to_mem(module->mod_start);
    elf_bytes = MULTIBOOT_MODULE_SIZE(*module);

    debug(SUBSYS_STARTUP, "load_init_image 0x%08lx %08x\n", elf_base, elf_bytes);
    debug(SUBSYS_STARTUP, "load_init_image %p %08x\n", elf_base, elf_bytes);

    err = elf_load(EM_ARM, startup_alloc_init, l2i,
    		elf_base, elf_bytes, init_ep);
    if (err_is_fail(err)) {
    	//err_print_calltrace(err);
    	panic("ELF load of " BSP_INIT_MODULE_NAME " failed!\n");
    }

    // TODO: Fix application linkage so that it's non-PIC.
    struct Elf32_Shdr* got_shdr =
        elf32_find_section_header_name((lvaddr_t)elf_base, elf_bytes, ".got");
    if (got_shdr)
    {
        *got_base = got_shdr->sh_addr;
    }
}

static void create_module_caps(struct spawn_state *st,
                               alloc_phys_func alloc_phys_fn)
{
    errval_t err;

    /* Create caps for multiboot modules */
    struct multiboot_modinfo *module =
        (struct multiboot_modinfo *)local_phys_to_mem(glbl_core_data->mods_addr);

    debug(SUBSYS_STARTUP, "glbl_core_data->mods_addr: 0x%"PRIxLPADDR" \n",
            glbl_core_data->mods_addr);

    // Allocate strings area
    lpaddr_t mmstrings_phys = alloc_phys_fn(BASE_PAGE_SIZE);
    lvaddr_t mmstrings_base = local_phys_to_mem(mmstrings_phys);
    lvaddr_t mmstrings = mmstrings_base;

    assert(st->modulecn_slot == 0);

    // create cap for strings area in first slot of modulecn
    struct cte *slot = caps_locate_slot(CNODE(st->modulecn),
                                           st->modulecn_slot++);
    err = caps_create_new(ObjType_Frame, mmstrings_phys, BASE_PAGE_BITS,
                          BASE_PAGE_BITS, slot);
    assert(err_is_ok(err));

    /* Walk over multiboot modules, creating frame caps */
    for (int i = 0; i < glbl_core_data->mods_count; i++) {
        struct multiboot_modinfo *m = &module[i];

        // Set memory regions within bootinfo
        struct mem_region *region =
            &bootinfo->regions[bootinfo->regions_length++];

        genpaddr_t remain = MULTIBOOT_MODULE_SIZE(*m);
        genpaddr_t base_addr = local_phys_to_gen_phys(m->mod_start);

        region->mr_type = RegionType_Module;
        region->mr_base = base_addr;
        region->mrmod_slot = st->modulecn_slot;  // first slot containing caps
        region->mrmod_size = remain;  // size of image _in bytes_
        region->mrmod_data = mmstrings - mmstrings_base; // offset of string in area

        // round up to page size for caps
        remain = ROUND_UP(remain, BASE_PAGE_SIZE);

        // Create max-sized caps to multiboot module in module cnode
        while (remain > 0) {
            assert((base_addr & BASE_PAGE_MASK) == 0);
            assert((remain & BASE_PAGE_MASK) == 0);

            // determine size of next chunk
            uint8_t block_size = bitaddralign(remain, base_addr);

            assert(st->modulecn_slot < (1UL << st->modulecn->cap.u.cnode.bits));
            // create as DevFrame cap to avoid zeroing memory contents
            err = caps_create_new(ObjType_DevFrame, base_addr, block_size,
                                  block_size,
                                  caps_locate_slot(CNODE(st->modulecn),
                                                   st->modulecn_slot++));
            assert(err_is_ok(err));

            // Advance by that chunk
            base_addr += ((genpaddr_t)1 << block_size);
            remain -= ((genpaddr_t)1 << block_size);
        }

        // Copy multiboot module string to mmstrings area
        strcpy((char *)mmstrings, MBADDR_ASSTRING(m->string));
        mmstrings += strlen(MBADDR_ASSTRING(m->string)) + 1;
        assert(mmstrings < mmstrings_base + BASE_PAGE_SIZE);
    }
}

/// Create physical address range or RAM caps to unused physical memory
static void create_phys_caps(lpaddr_t unused_start_addr,
        lpaddr_t max_allowed_addr, struct spawn_state *spawn_state)
{
    errval_t err;

    /* Walk multiboot MMAP structure, and create appropriate caps for memory */
    char *mmap_addr = MBADDR_ASSTRING(glbl_core_data->mmap_addr);
    genpaddr_t last_end_addr = 0;

    for(char *m = mmap_addr; m < mmap_addr + glbl_core_data->mmap_length;)
    {
        struct multiboot_mmap *mmap = (struct multiboot_mmap * SAFE)TC(m);

        if (last_end_addr >= unused_start_addr
                && mmap->base_addr > last_end_addr)
        {
            /* we have a gap between regions. add this as a physaddr range */
            err = create_caps_to_cnode(last_end_addr,
                    mmap->base_addr - last_end_addr,
                    RegionType_PhyAddr, spawn_state, bootinfo);
            assert(err_is_ok(err));
        }

        if (mmap->type == MULTIBOOT_MEM_TYPE_RAM)
        {

            genpaddr_t base_addr = mmap->base_addr;
            genpaddr_t end_addr  = base_addr + mmap->length;

            // Ensuring that init gets only part of memory which is allowed
            // for this core
            assert(base_addr < max_allowed_addr);
            if (end_addr > max_allowed_addr) {
                end_addr = max_allowed_addr;
            }

            // only map RAM which is greater than unused_start_addr
            if (end_addr > local_phys_to_gen_phys(unused_start_addr))
            {
                if (base_addr < local_phys_to_gen_phys(unused_start_addr)) {
                    base_addr = local_phys_to_gen_phys(unused_start_addr);
                }

                assert(end_addr >= base_addr);
                err = create_caps_to_cnode(base_addr, end_addr - base_addr,
                        RegionType_Empty, spawn_state, bootinfo);
                assert(err_is_ok(err));
            }
            //last_end_addr = mmap->base_addr + mmap->length;
            last_end_addr = end_addr;
        }
        else {
            if (mmap->base_addr > local_phys_to_gen_phys(unused_start_addr))
            {
                /* XXX: The multiboot spec just says that mapping types other than
                 * RAM are "reserved", but GRUB always maps the ACPI tables as type
                 * 3, and things like the IOAPIC tend to show up as type 2 or 4,
                 * so we map all these regions as platform data
                 */
                assert(mmap->base_addr > local_phys_to_gen_phys(unused_start_addr));
                err = create_caps_to_cnode(mmap->base_addr, mmap->length,
                        RegionType_PlatformData, spawn_state, bootinfo);
                assert(err_is_ok(err));
            }
            last_end_addr = mmap->base_addr + mmap->length;
        }
        m += mmap->size + 4;
    }

    // Assert that we have some physical address space
    assert(last_end_addr != 0);

    if (last_end_addr < PADDR_SPACE_SIZE)
    {
        /*
         * FIXME: adding the full range results in too many caps to add
         * to the cnode (and we can't handle such big caps in user-space
         * yet anyway) so instead we limit it to something much smaller
         */
        genpaddr_t size = PADDR_SPACE_SIZE - last_end_addr;
        const genpaddr_t phys_region_limit = 1ULL << 32; // PCI implementation limit
        if (last_end_addr > phys_region_limit) {
            size = 0; // end of RAM is already too high!
        } else if (last_end_addr + size > phys_region_limit) {
            size = phys_region_limit - last_end_addr;
        }
        err = create_caps_to_cnode(last_end_addr, size,
                RegionType_PhyAddr, spawn_state, bootinfo);
        assert(err_is_ok(err));
    }
}


/*
 * \brief Initialzie page tables
 *
 * This includes setting up page tables for the init process.
 */
static void init_page_tables(struct spawn_state *spawn_state)
{
    // Create page table for init
    if(hal_cpu_is_bsp()) {
        init_l1 =  (union arm_l1_entry *)local_phys_to_mem(
                bsp_alloc_phys_aligned(INIT_L1_BYTES, ARM_L1_ALIGN));
        memset(init_l1, 0, INIT_L1_BYTES);

        init_l2 = (union arm_l2_entry *)local_phys_to_mem(
                bsp_alloc_phys_aligned(INIT_L2_BYTES, ARM_L2_ALIGN));
        memset(init_l2, 0, INIT_L2_BYTES);
    } else {
        init_l1 =  (union arm_l1_entry *)local_phys_to_mem(
                bsp_alloc_phys_aligned(INIT_L1_BYTES, ARM_L1_ALIGN));
        memset(init_l1, 0, INIT_L1_BYTES);

        init_l2 = (union arm_l2_entry *)local_phys_to_mem(
                bsp_alloc_phys_aligned(INIT_L2_BYTES, ARM_L2_ALIGN));
        memset(init_l2, 0, INIT_L2_BYTES);
    }

    debug(SUBSYS_STARTUP, "init_page_tables done: init_l1=%p init_l2=%p\n",
            init_l1, init_l2);

    /* Map pagetables into page CN */
    int pagecn_pagemap = 0;

    /*
     * ARM has:
     *
     * L1 has 4096 entries (16KB).
     * L2 Coarse has 256 entries (256 * 4B = 1KB).
     *
     * CPU driver currently fakes having 1024 entries in L1 and
     * L2 with 1024 entries by treating a page as 4 consecutive
     * L2 tables and mapping this as a unit in L1.
     */
    caps_create_new(ObjType_VNode_ARM_l1,
                    mem_to_local_phys((lvaddr_t)init_l1),
                    vnode_objbits(ObjType_VNode_ARM_l1), 0,
                    caps_locate_slot(CNODE(spawn_state->pagecn), pagecn_pagemap++)
                    );

    //STARTUP_PROGRESS();

    // Map L2 into successive slots in pagecn
    size_t i;
    for (i = 0; i < INIT_L2_BYTES / BASE_PAGE_SIZE; i++) {
        size_t objbits_vnode = vnode_objbits(ObjType_VNode_ARM_l2);
        assert(objbits_vnode == BASE_PAGE_BITS);
        caps_create_new(
                        ObjType_VNode_ARM_l2,
                        mem_to_local_phys((lvaddr_t)init_l2) + (i << objbits_vnode),
                        objbits_vnode, 0,
                        caps_locate_slot(CNODE(spawn_state->pagecn), pagecn_pagemap++)
                        );
    }

    /*
     * Initialize init page tables - this just wires the L1
     * entries through to the corresponding L2 entries.
     */
    STATIC_ASSERT(0 == (INIT_VBASE % ARM_L1_SECTION_BYTES), "");
    for (lvaddr_t vaddr = INIT_VBASE;
         vaddr < INIT_SPACE_LIMIT;
         vaddr += ARM_L1_SECTION_BYTES) {
        uintptr_t section = (vaddr - INIT_VBASE) / ARM_L1_SECTION_BYTES;
        uintptr_t l2_off = section * ARM_L2_TABLE_BYTES;
        lpaddr_t paddr = mem_to_local_phys((lvaddr_t)init_l2) + l2_off;
        paging_map_user_pages_l1((lvaddr_t)init_l1, vaddr, paddr);
    }

    printk(LOG_NOTE, "Calling paging_context_switch with address = %"PRIxLVADDR"\n",
           mem_to_local_phys((lvaddr_t) init_l1));
    paging_context_switch(mem_to_local_phys((lvaddr_t)init_l1));
}

static struct dcb *spawn_init_common(const char *name,
                                     int argc, const char *argv[],
                                     lpaddr_t bootinfo_phys,
                                     alloc_phys_func alloc_phys_fn,
                                     struct cte *rootcn,
                                     struct spawn_state *spawn_state)
{
    debug(SUBSYS_STARTUP, "spawn_init_common %s\n", name);

    lvaddr_t paramaddr;
    struct dcb *init_dcb = spawn_module(spawn_state, rootcn, name,
                                        argc, argv,
                                        bootinfo_phys, INIT_ARGS_VBASE,
                                        alloc_phys_fn, &paramaddr);

    debug(SUBSYS_STARTUP, "after spawn_module\n");
    init_page_tables(spawn_state);

    debug(SUBSYS_STARTUP, "about to call mem_to_local_phys with lvaddr=%"PRIxLVADDR"\n",
           init_l1);

    init_dcb->vspace = mem_to_local_phys((lvaddr_t)init_l1);

    spawn_init_map(init_l2, INIT_VBASE, INIT_ARGS_VBASE,
                   spawn_state->args_page, ARGS_SIZE, INIT_PERM_RW);


    // Map dispatcher
    spawn_init_map(init_l2, INIT_VBASE, INIT_DISPATCHER_VBASE,
                   mem_to_local_phys(init_dcb->disp), DISPATCHER_SIZE,
                   INIT_PERM_RW);

    struct dispatcher_shared_generic *disp
        = get_dispatcher_shared_generic(init_dcb->disp);
    struct dispatcher_shared_arm *disp_arm
        = get_dispatcher_shared_arm(init_dcb->disp);

    /* Initialize dispatcher */
    disp->disabled = true;
    strncpy(disp->name, argv[0], DISP_NAME_LEN);

    /* tell init the vspace addr of its dispatcher */
    disp->udisp = INIT_DISPATCHER_VBASE;

    disp_arm->enabled_save_area.named.r0   = paramaddr;
    disp_arm->enabled_save_area.named.cpsr = ARM_MODE_USR | CPSR_F_MASK;
    disp_arm->enabled_save_area.named.rtls = INIT_DISPATCHER_VBASE;
    disp_arm->disabled_save_area.named.rtls = INIT_DISPATCHER_VBASE;

    return init_dcb;
}

// copied from: lib/newlib/newlib/libc/unix/basename.c
static char *basename(const char *path)
{
    char *p;
    if( path == NULL || *path == '\0' )
        return ".";
    p = (char*)path + strlen(path) - 1;
    while( *p == '/' ) {
        if( p == path )
            return (char*)path;
        *p-- = '\0';
    }
    while( p >= path && *p != '/' )
        p--;
    return p + 1;
}


struct dcb *spawn_bsp_init(const char *name, alloc_phys_func alloc_phys_fn,
        struct cte *rootcn, struct spawn_state *spawn_state)
{
    debug(SUBSYS_STARTUP, "spawn_bsp_init\n");

    /* Allocate bootinfo */
    lpaddr_t bootinfo_phys = alloc_phys_fn(BOOTINFO_SIZE);
    memset((void *)local_phys_to_mem(bootinfo_phys), 0, BOOTINFO_SIZE);

    /* Construct cmdline args */
    char bootinfochar[16];
    snprintf(bootinfochar, sizeof(bootinfochar), "%u", INIT_BOOTINFO_VBASE);
    char *bname = basename(name);
    const char *argv[] = { bname, bootinfochar };
    int argc = 2;

    struct dcb *init_dcb = spawn_init_common(name, argc, argv, bootinfo_phys,
            alloc_phys_fn, rootcn, spawn_state);

    debug(SUBSYS_STARTUP, "setting dispatcher_shared_arm:%p %p\n", init_dcb,
            init_dcb->disp);

    // Map bootinfo
    spawn_init_map(init_l2, INIT_VBASE, INIT_BOOTINFO_VBASE,
                   bootinfo_phys, BOOTINFO_SIZE, INIT_PERM_RW);


    struct startup_l2_info l2_info = { init_l2, INIT_VBASE };

    genvaddr_t init_ep, got_base = 0;
    load_init_image(&l2_info, name, &init_ep, &got_base);

    struct dispatcher_shared_arm *disp_arm
        = get_dispatcher_shared_arm(init_dcb->disp);
    disp_arm->enabled_save_area.named.r10  = got_base;
    disp_arm->got_base = got_base;

    disp_arm->disabled_save_area.named.pc   = init_ep;
    disp_arm->disabled_save_area.named.cpsr = ARM_MODE_USR | CPSR_F_MASK;
    disp_arm->disabled_save_area.named.r10  = got_base;

    /* Create caps for init to use */
    if (strncmp(bname, "init", 4) == 0) {
        printk(LOG_NOTE, "we're init and get the module, phys, and devmem caps\n");
        create_module_caps(spawn_state, alloc_phys_fn);
        lpaddr_t free_ram_start = alloc_phys_fn(0);
        printk(LOG_NOTE, "creating caps for %"PRIxLPADDR" -- %"PRIxLPADDR"\n",
                free_ram_start, core_local_alloc_end);
        create_phys_caps(free_ram_start, core_local_alloc_end, spawn_state);

        /*
         * we create the capability to the devices at this stage and store it
         * in the TASKCN_SLOT_IO, where on x86 the IO capability is stored for
         * device access on PCI. PCI is not available on the pandaboard so this
         * should not be a problem.
         */
        struct cte *iocap = caps_locate_slot(CNODE(spawn_state->taskcn), TASKCN_SLOT_IO);
        errval_t  err = caps_create_new(ObjType_DevFrame, 0x40000000, 30, 30, iocap);
        assert(err_is_ok(err));
    }

    /* Fill bootinfo struct */
    bootinfo->mem_spawn_core = KERNEL_IMAGE_SIZE; // Size of kernel
     // start address of module list
    bootinfo->mod_start = glbl_core_data->mods_addr; // ELF module list addr
    bootinfo->mod_count = glbl_core_data->mods_count; // number of ELF modules
    bootinfo->mmap_addr = glbl_core_data->mmap_addr;
    bootinfo->mmap_length = glbl_core_data->mmap_length;

    printk(LOG_NOTE, "spawn_bsp_init done!\n");
    return init_dcb;
}


void arm_kernel_startup(void)
{
    printk(LOG_NOTE, "arm_kernel_startup entered \n");

    /* Initialize the core_data */
    /* Used when bringing up other cores, must be at consistent global address
     * seen by all cores */
    struct arm_core_data *core_data
        = (void *)((lvaddr_t)&kernel_first_byte - BASE_PAGE_SIZE);

    struct dcb *init_dcb;

    if(hal_cpu_is_bsp())
    {
        debug(SUBSYS_STARTUP, "Doing BSP related bootup \n");

    	/* Initialize the location to allocate phys memory from */
        core_local_alloc_start = glbl_core_data->start_free_ram;
        core_local_alloc_end = PHYS_MEMORY_START + ram_size;

//#if MILESTONE == 3
        // Bring up memory consuming process
        struct spawn_state memeater_st;
        struct dcb *memeater_dcb;
        memset(&memeater_st, 0, sizeof(struct spawn_state));
        static struct cte memeater_rootcn; // gets put into mdb
        memeater_dcb = spawn_bsp_init(ADDTITIONAL_MODULE_NAME,
                                      bsp_alloc_phys, &memeater_rootcn, &memeater_st);
//#endif

        // Bring up init
        struct spawn_state init_st;
        memset(&init_st, 0, sizeof(struct spawn_state));
        static struct cte init_rootcn; // gets put into mdb
        init_dcb = spawn_bsp_init(BSP_INIT_MODULE_NAME, bsp_alloc_phys,
                                  &init_rootcn, &init_st);

//#if MILESTONE == 3
        // TODO (milestone 3): create endpoints for domains:
        // 1) selfep for each domain -- retype dcb cap into TASKCN_SLOT_SELFEP
        // DONE
        // 2) create init's receive ep -- mint init's self ep into
        //    TASKCN_SLOT_INITEP & use FIRSTEP_{OFFSET,BUFLEN} as arguments
        //    for minting.
        // DONE
        // 3) copy init's receive ep into all other domains'
        //    TASKCN_SLOT_INITEP.
        // DONE
         errval_t err;

        /*
        errval_t caps_retype(enum objtype type, size_t objbits,
                     struct capability *dest_cnode, cslot_t dest_slot,
                     struct cte *src_cte, bool from_monitor)
        */
        // 1
        struct cte *dispatcher_cap = caps_locate_slot(CNODE(init_st.taskcn), TASKCN_SLOT_DISPATCHER);
        assert(dispatcher_cap != (struct cte *) NULL);
        err = caps_retype(ObjType_EndPoint, 0, &(init_st.taskcn->cap), 
                            TASKCN_SLOT_SELFEP, dispatcher_cap, true);
        if (err != SYS_ERR_OK) {
            printf("INIT.1 Error Code: %d\n", err);
        }
        assert(err_is_ok(err));

        dispatcher_cap = caps_locate_slot(CNODE(memeater_st.taskcn), TASKCN_SLOT_DISPATCHER);
        assert(dispatcher_cap != (struct cte *) NULL);
        err = caps_retype(ObjType_EndPoint, 0, &(memeater_st.taskcn->cap), 
                            TASKCN_SLOT_SELFEP, dispatcher_cap, true);
        if (err != SYS_ERR_OK) {
            printf("MEMEATER.1 Error Code: %d\n", err);
        }
        assert(err_is_ok(err)); 
        debug(LOG_NOTE, "Dispatcher Cap retyped to Endpoint Cap.\n");

        // 2
        struct cte *init_ep_cap = caps_locate_slot(CNODE(init_st.taskcn), TASKCN_SLOT_SELFEP);
        assert(init_ep_cap != (struct cte *) NULL);
        err = caps_copy_to_cnode(init_st.taskcn, TASKCN_SLOT_INITEP, init_ep_cap,
                                    true, FIRSTEP_OFFSET, FIRSTEP_BUFLEN);
        if (err != SYS_ERR_OK) {
            printf("INIT.2 Error Code: %d\n", err);
        }
        assert(err_is_ok(err));
        /*
        errval_t caps_copy_to_cnode(struct cte *dest_cnode_cte, cslot_t dest_slot,
                            struct cte *src_cte, bool mint, uintptr_t param1,
                            uintptr_t param2);
        */
        // 3
        struct cte *init_recv_ep = caps_locate_slot(CNODE(init_st.taskcn), TASKCN_SLOT_INITEP);
        assert(init_recv_ep != (struct cte *) NULL);
        err = caps_copy_to_cnode(memeater_st.taskcn, TASKCN_SLOT_INITEP, init_recv_ep,
                                    false, 0, 0);
        if (err != SYS_ERR_OK) {
            printf("INIT.3 Error Code: %d\n", err);
        }
        assert(err_is_ok(err));
        debug(LOG_NOTE, "Minted Init EP to other domain INIT_EP Slots.\n");
        printk(LOG_NOTE, "EP Capss created and Buffers minted accordingly...\n");
        
//#endif
    } else {
        debug(SUBSYS_STARTUP, "Doing non-BSP related bootup \n");
        init_dcb = NULL;

    	my_core_id = core_data->dst_core_id;

        // TODO (multicore milestone): setup init domain for core 1

    	uint32_t irq = gic_get_active_irq();
    	gic_ack_irq(irq);
    }

    // Should not return
    printk(LOG_NOTE, "Calling dispatch from arm_kernel_startup, start address is=%"
            PRIxLVADDR"\n",
            get_dispatcher_shared_arm(init_dcb->disp)->enabled_save_area.named.r0);
    dispatch(init_dcb);
    panic("Error spawning init!");

}

