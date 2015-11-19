#include <barrelfish/barrelfish.h>
#include <barrelfish/cspace.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <barrelfish/debug.h>
#include <barrelfish/lmp_chan_arch.h>
#include <barrelfish/lmp_chan.h>
#include <barrelfish/aos_rpc.h>
#include <barrelfish/paging.h>
#include <omap44xx_map.h>

#define GPIO1_BASE 0x4a310000

static volatile uint32_t *gpio1_oe;
static volatile uint32_t *gpio1_dataout;

void blink_me(bool on);
void blink_me(bool on)
{
    uint32_t bitmask = 1<<8;
    
    // Output enable
    *gpio1_oe &= ~bitmask;
    
    // Enable/disable led
    if(on) {
        *gpio1_dataout |= bitmask;
    } else {
        *gpio1_dataout &= ~bitmask;
    }
    
    // Output disable
    *gpio1_oe |= bitmask;
}

void stall(float secs);
void stall(float secs) 
{
    unsigned long us = (int) (float)(1UL << 26)*3*secs;

    while (us-- > 0) 
    {
        __asm volatile("nop");
    }
}

void set_gpio1_registers(lvaddr_t base);
void set_gpio1_registers(lvaddr_t base)
{
    gpio1_oe      = (uint32_t *) (base + 0x134);
    gpio1_dataout = (uint32_t *) (base + 0x13c);
}

int main(int argc, char *argv[])
{
    debug_printf("blink started\n");
    
    int32_t no_of_blinks = (argc > 1) ? atoi(argv[1]) : 5;
    float blink_rate = (float) (argc > 2) ? atoi(argv[2]) : 1;
    
    
    debug_printf("Getting IO Cap from Init...\n");
    errval_t err;
    struct capref retcap;
    size_t retlen;
    err = aos_rpc_get_dev_cap(&local_rpc, OMAP44XX_MAP_L4_PER_UART3,
        OMAP44XX_MAP_L4_PER_UART3_SIZE, &retcap, &retlen);

    if (err_is_fail(err)) {
        debug_printf("Failed to get IO Cap from init... %s\n");
        err_print_calltrace(err);
        abort();
    }

    size_t offset = GPIO1_BASE - 0x40000000;
    lvaddr_t uart_addr = (1UL << 28)*3;
    err = paging_map_user_device(get_current_paging_state(), uart_addr,
                            retcap, offset, OMAP44XX_MAP_L4_PER_UART3_SIZE,
                            VREGION_FLAGS_READ_WRITE_NOCACHE);
                            
    set_gpio1_registers(uart_addr);
    
    debug_printf("cap_io mapped OK.\n");

    debug_printf("start blinking, G!\n");
    while(no_of_blinks-- > 0) {
        blink_me(true);
        stall(1./blink_rate);

        blink_me(false);
        stall(1./blink_rate);
    }
        
    return 0;
}
