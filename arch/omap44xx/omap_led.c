#include <kernel.h>
#include <paging_kernel_arch.h>
#include <omap44xx_led.h>

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
    // TODO: Output enable

    // TODO: you'll want to change the infinite loop here for milestone 1.
    while (true) {
        // TODO: Write data out

        // TODO: wait for approximately 1 second.
    }
}


