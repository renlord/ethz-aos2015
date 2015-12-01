#include "blink.h"

static volatile uint32_t *gpio1_oe;
static volatile uint32_t *gpio1_dataout;

static struct periodic_event pe;

static void set_gpio1_registers(lvaddr_t base);
// static void stall(float secs);
// static void blink_led(void);
// static void led_toggle(bool t);

static void blink_led(void)
{
    uint32_t bitmask = 1<<8;

    // Output enable
    *gpio1_oe &= ~bitmask;
    
    // Enable/disable led
    if ((*gpio1_dataout & bitmask) == 0) {
        *gpio1_dataout |= bitmask;
    } else {
        *gpio1_dataout &= ~bitmask;
    }
    
    // Output disable
    *gpio1_oe |= bitmask;

    errval_t err = event_dispatch(get_default_waitset());
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "failed to dispatch event\n");
        err_print_calltrace(err);
    }
}

// static void led_toggle(bool t)
// {
//     uint32_t bitmask = 1<<8;

//     // Output enable
//     *gpio1_oe &= ~bitmask;
    
//     *gpio1_dataout = t ? (*gpio1_dataout |= bitmask) : 
//                             (*gpio1_dataout &= ~bitmask);

//     // Output disable
//     *gpio1_oe |= bitmask;
// }

// static void stall(float secs) 
// {
//     unsigned long us = (int) (float)(1UL << 26)*3*secs;

//     while (us-- > 0) 
//     {
//         __asm volatile("nop");
//     }
// }

static void set_gpio1_registers(lvaddr_t base)
{
    gpio1_oe      = (uint32_t *) (base + 0x134);
    gpio1_dataout = (uint32_t *) (base + 0x13c);
}

int main(int argc, char *argv[])
{
    printf("%s, pid: %u\r\n", disp_name(), disp_get_domain_id());
    
    // while(true){
    //     if(i++ % 30000000 == 0){
    //         aos_rpc_send_string(&local_rpc, "special blink");
    //         event_dispatch(get_default_waitset());
    //         // debug_printf("blink still alive...\n");
    //     }
    // }
    
    // int32_t no_of_blinks = (argc > 1) ? atoi(argv[1]) : 5;
    delayus_t blink_rate = ((argc > 2) ? atoi(argv[2]) : 1) * 10000; 

    printf("blink_rate set to: %u\r\n", blink_rate);
    
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
    printf("dev cap received, dev mapped. OK\r\n");
    
    size_t offset = GPIO1_BASE - 0x40000000;
    lvaddr_t uart_addr = (1UL << 28)*3;
    err = paging_map_user_device(get_current_paging_state(), uart_addr,
                            retcap, offset, OMAP44XX_MAP_L4_PER_UART3_SIZE,
                            VREGION_FLAGS_READ_WRITE_NOCACHE);
                            
    set_gpio1_registers(uart_addr);
    printf("user device registers set. OK\r\n");

    err = periodic_event_create(&pe, get_default_waitset(), blink_rate,  
                                    MKCLOSURE((void *) blink_led, NULL));
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "failed to register periodic event!\n");
        err_print_calltrace(err);
    } else {
        debug_printf("periodic event registered.\r\n");
    }
    
    err = event_dispatch(get_default_waitset());
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "failed to dispatch event\n");
        err_print_calltrace(err);
    } 
    
    // assert(argc == 1);
    // led_toggle(argv[1]);

    return 0;
}
