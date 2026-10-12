#include "ssd1306.h"

#include "hardware/gpio.h"
#include "hardware/i2c.h"

#include "config.h"

static void cmd(uint8_t c) {
    uint8_t buf[2] = {0x00, c};
    i2c_write_blocking(OLED_I2C, OLED_ADDR, buf, 2, false);
}

void ssd1306_init(void) {
    i2c_init(OLED_I2C, OLED_I2C_HZ);
    gpio_set_function(PIN_OLED_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_OLED_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_OLED_SDA);
    gpio_pull_up(PIN_OLED_SCL);

    static const uint8_t init_seq[] = {
        0xAE,       // display off
        0xD5, 0x80, // clock divide
        0xA8, 0x3F, // multiplex 64
        0xD3, 0x00, // display offset
        0x40,       // start line 0
        0x8D, 0x14, // charge pump on
        0x20, 0x00, // horizontal addressing mode
        0xA1,       // segment remap
        0xC8,       // COM scan dec
        0xDA, 0x12, // COM pins
        0x81, 0xCF, // contrast
        0xD9, 0xF1, // precharge
        0xDB, 0x40, // VCOM detect
        0xA4,       // resume from RAM
        0xA6,       // normal (not inverted)
        0xAF,       // display on
    };
    for (unsigned i = 0; i < sizeof(init_seq); i++) cmd(init_seq[i]);
}

void ssd1306_show(const uint8_t *fb) {
    cmd(0x21); cmd(0); cmd(127); // column range
    cmd(0x22); cmd(0); cmd(7);   // page range
    // Send the data-stream control byte without STOP, then the framebuffer
    // straight out of it: no 1 KB copy per frame on core 1.
    static const uint8_t ctrl = 0x40;
    i2c_write_blocking(OLED_I2C, OLED_ADDR, &ctrl, 1, true);
    i2c_write_blocking(OLED_I2C, OLED_ADDR, fb, OLED_FB_SIZE, false);
}
