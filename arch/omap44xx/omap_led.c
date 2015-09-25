#include <kernel.h>
#include <paging_kernel_arch.h>
#include <omap44xx_led.h>

#include <math.h>

#define GPIO_OE 0x4A310134
#define GPIO_DATAOUT 0x4A31013C


static volatile uint32_t *gpio_oe = (uint32_t *)0x4A310134;
static volatile uint32_t *gpio_dataout = (uint32_t *)0x4A31013C;
//
// LED section
//

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


/*
 * Flash the LED On the pandaboard
 * TODO: implement this function for milestone 0
 */
__attribute__((noreturn))
void led_flash(void)
{
    int bit = 1<<8;
    int inverse = (0xFFFFFFFF ^ bit);
    
    *gpio_oe &= inverse;

    int i = 0;
    while(true) {
        // Write data out
        if(i++ % 2 == 0)
            *gpio_dataout |= bit;
        else
            *gpio_dataout &= inverse;
      
        for(int j = 0; j < 1<<11; j++)
            printf("i: %d\n", i);
    }
}


