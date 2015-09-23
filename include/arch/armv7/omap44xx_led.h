#ifndef OMAP44XX_LED_H
#define OMAP44XX_LED_H

void led_map_register(void);
void led_set_state(bool new_state);
void led_flash(void);

#endif // OMAP44XX_LED_H
