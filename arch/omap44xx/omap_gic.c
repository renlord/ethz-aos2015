#include <kernel.h>
#include <arm.h>
#include <arm_hal.h>
#include <cp15.h>
#include <paging_kernel_arch.h>

/*
 * interrupt setup
 */

#define DIST_OFFSET     0x1000 // Interrupt Distributor
#define CPU_OFFSET 	0x0100 // Interrupt controller interface


static volatile uint32_t *gicd_dcr = NULL;
static volatile uint32_t *gicd_typer = NULL;

static volatile uint32_t *gicc_icr = NULL;
static volatile uint32_t *gicc_pmr = NULL;
static volatile uint32_t *gicc_bpr = NULL;


// relevant interrupt distributor registers
// c.f. ARM Cortex A9 MPCore TRM r2p0 Table 3-1
static void set_gic_registers(lvaddr_t dist_base, lvaddr_t cpu_base)
{
    gicd_dcr = (uint32_t*)dist_base;
    gicd_typer = (uint32_t*)(dist_base + 0x4);

    gicc_icr = (uint32_t*)(cpu_base);
    gicc_pmr = (uint32_t*)(cpu_base + 0x4);
    gicc_bpr = (uint32_t*)(cpu_base + 0x8);
}
static uint32_t gic_config; // contents of register GICD_TYPER

static uint32_t it_num_lines;

/*
 * There are three types of interrupts
 * 1) Software generated Interrupts (SGI) - IDs 0-15
 * 2) Private Peripheral Interrupts (PPI) - IDs 16-31
 * 3) Shared Peripheral Interrups (SPI) - IDs 32...
 */
void gic_init(void)
{
    lpaddr_t periphbase = cp15_read_cbar();
    printf("map_private_memory_region: PERIPHBASE=%x\n", periphbase);

    // According to ARM Cortex A9 MPCore TRM, this is two contiguous 4KB pages
    // We map a section (1MB) anyway
    lvaddr_t private_memory_region = paging_map_device(periphbase, ARM_L1_SECTION_BYTES);

    // paging_map_device returns an address pointing to the beginning of
    // a section, need to add the offset for within the section again
    private_memory_region += (periphbase & ARM_L1_SECTION_MASK);

    printf("private memory region (phy) %x, private memory region (virt) %x\n",
           periphbase, private_memory_region);

    // setting base addresses for ditributor & cpu interfaces
    lvaddr_t dist_base = private_memory_region + DIST_OFFSET;
    lvaddr_t cpu_base = private_memory_region + CPU_OFFSET;

    set_gic_registers(dist_base, cpu_base);

    assert(gicd_dcr);
    assert(gicd_typer);

    assert(gicc_icr);
    assert(gicc_pmr);
    assert(gicc_bpr);

    // read GIC configuration
    gic_config = *gicd_typer;

    // ARM GIC TRM, 3.1.2
    // This is the number of ICDISERs, i.e. #SPIs
    // Number of SIGs (0-15) and PPIs (16-31) is fixed
    // Why (x+1)*32
    // ARM GIC TRM v2, 4.3.2 Interrupt Controller Type Register
    // bits 0-4 are ITLinesNumber
    uint32_t it_num_lines_tmp = gic_config & 0x1f;
    it_num_lines = 32*(it_num_lines_tmp + 1);

    printf("GIC: %d interrupt lines detected\n", it_num_lines_tmp);

    // set priority mask of cpu interface, currently set to lowest priority
    // to accept all interrupts
    // ARM GIC TRM v2, 4.4.2
    // set bits 0-7 in Interrupt Priority Mask Register (GICC_PMR)
    *gicc_pmr = 0xff;

    // set binary point to define split of group- and subpriority
    // currently we allow for 8 subpriorities
    *gicc_bpr = 0x2;

    // enable interrupt forwarding to processor
    *gicc_icr = 0x1;

    // Distributor:
    // enable interrupt forwarding from distributor to cpu interface
    *gicd_dcr = 0x1;
    printf("gic_init: done\n");
}

