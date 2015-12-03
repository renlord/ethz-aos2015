#include "blink.h"

#ifdef BLINK_DEBUG
# define BLINK_PRINT(x) printf x
#else
# define BLINK_PRINT(x) do {} while (0)
#endif

static volatile uint32_t *gpio1_oe;
static volatile uint32_t *gpio1_dataout;

static struct periodic_event pe;

static void set_gpio1_registers(lvaddr_t base);

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

}

static void set_gpio1_registers(lvaddr_t base)
{
    gpio1_oe      = (uint32_t *) (base + 0x134);
    gpio1_dataout = (uint32_t *) (base + 0x13c);
}

int main(int argc, char *argv[])
{
    BLINK_PRINT(("%s, pid: %u\r\n", disp_name(), disp_get_domain_id()));
    
    uint32_t blink_rate = (argc > 1) ? atoi(argv[1]) : 1; 
    uint32_t no_of_blinks = (argc > 2) ? atoi(argv[2]) : 10; 
    
    delayus_t delay = 1000.0/(float)blink_rate;
    
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
    BLINK_PRINT("dev cap received, dev mapped. OK\r\n");
    
    size_t offset = GPIO1_BASE - 0x40000000;
    lvaddr_t uart_addr = (1UL << 28)*3;
    err = paging_map_user_device(get_current_paging_state(), uart_addr,
                            retcap, offset, OMAP44XX_MAP_L4_PER_UART3_SIZE,
                            VREGION_FLAGS_READ_WRITE_NOCACHE);
                            
    set_gpio1_registers(uart_addr);
    BLINK_PRINT("user device registers set. OK\r\n");

    err = periodic_event_create(&pe, get_default_waitset(), delay,  
                                    MKCLOSURE((void *) blink_led, NULL));
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "failed to register periodic event!\n");
        err_print_calltrace(err);
    } else {
        BLINK_PRINT("periodic event registered.\r\n");
    }
    
    for(uint32_t i = 0; i < no_of_blinks; i++) {
        err = event_dispatch(get_default_waitset());
        if (err_is_fail(err)) {
            DEBUG_ERR(err, "failed to dispatch event\n");
            err_print_calltrace(err);
        }
    }
    
    err = periodic_event_cancel(&pe);
    if(err_is_fail(err)){
        err_print_calltrace(err);
        abort();
    }
    
    BLINK_PRINT("blink exiting\n");

    err = aos_rpc_send_string(&local_rpc, "bye");
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "fail to say bye\n");
        err_print_calltrace(err);
    }

    debug_printf("blink entering infinite loop\n");
    while (true);
    
    return 0;
}
