#include <linux/delay.h>
#include <linux/i8042.h>

#define I8042_KBD_IRQ  1

static int i8042_command_reg = 0x64;
static int i8042_data_reg = 0x60;

#define I8042_COMMAND_REG       i8042_command_reg
#define I8042_STATUS_REG        i8042_command_reg
#define I8042_DATA_REG          i8042_data_reg

static inline int i8042_read_data(void)
{
        return inb(I8042_DATA_REG);
}

static inline int i8042_read_status(void)
{
        return inb(I8042_STATUS_REG);
}

static inline void i8042_write_data(int val)
{
        outb(val, I8042_DATA_REG);
}

static inline void i8042_write_command(int val)
{
        outb(val, I8042_COMMAND_REG);
}

/*
 * i8042_led_blink() will turn the keyboard LEDs on or off
 * Note that DELAY has a limit of 10ms so we will not get stuck here
 * waiting for KBC to free up even if KBD interrupt is off
 *
 * @state: the control value for all 3 leds. 
 *         i8042_led_blink(I8042_LED_NUMLOCK | I8042_LED_CAPSLOCK) would
 *         turn on numlock and capslock.
 */

#define DELAY do { mdelay(1); if (++delay > 10) return delay; } while(0)

#define I8042_LED_SCROLLLOCK 0x01
#define I8042_LED_NUMLOCK    0x02
#define I8042_LED_CAPSLOCK   0x04

static long i8042_led_blink(char state)
{
        long delay = 0;

        while (i8042_read_status() & I8042_STR_IBF)
                DELAY;
        pr_debug("%02x -> i8042 (blink)\n", 0xed);
        i8042_write_data(0xed); /* set leds */
        DELAY;
        while (i8042_read_status() & I8042_STR_IBF)
                DELAY;
        DELAY;
        pr_debug("%02x -> i8042 (blink)\n", state);
        i8042_write_data(state);
        DELAY;
        return delay;
}
