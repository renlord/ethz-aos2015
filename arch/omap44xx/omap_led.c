#include <kernel.h>
#include <paging_kernel_arch.h>
#include <omap44xx_led.h>

#include <math.h>

#define GPIO_OE 0x4A310134
#define GPIO_DATAOUT 0x4A31013C

//
// LED section
//

#define GPIO1_BASE 0x4a310000

static volatile uint32_t *gpio1_oe = (uint32_t *) (GPIO1_BASE + 0x134);
static volatile uint32_t *gpio1_dataout = (uint32_t *) (GPIO1_BASE + 0x13c);

void delay(unsigned int ticks);

/*
 * You will need to implement this function for the milestone 1 extra
 * challenge.
 */
void led_map_register(void)
{
    // TODO: remap GPIO registers in newly setup address space and ensure
    // that led_flash and co use the new address locations.
}
/*
 * Enable/disable led
 * TODO: This function might be useful for the extra challenge in
 * milestone 1.
 */
void led_set_state(bool new_state)
{
}
// A quick and dirty delay routine I learnt from the World Wide Web.
void delay(unsigned int ticks) 
{
    unsigned long us = 1000 * ticks;

    while (us--) 
    {
        __asm volatile("nop");
    }
}


/*
 * Flash the LED On the pandaboard
 * TODO: implement this function for milestone 0
 */
void led_flash(void)
{
    // Bitmasks for turning bit 8 on and off
    int bitmask = 1<<8;
    int bitmask_inv = (0xFFFFFFFF ^ bitmask);

    // I have to cheat a little as there's no timer and i have no idea
    // how to access the CPU cycles
    // TODO: Output enable
    *gpio1_oe &= bitmask_inv;

    // TODO: you'll want to change the infinite loop here for milestone 1.
    printf("LED Turning ON\n");
    // TODO: Write data out
    *gpio1_dataout |= bitmask;
    // TODO: wait for approximately 1 second.
    delay(225000);
    printf("LED Turning OFF\n");
    *gpio1_dataout &= bitmask_inv;
    delay(225000);
}


