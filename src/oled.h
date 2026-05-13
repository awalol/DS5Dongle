#ifndef OLED_H
#define OLED_H

#include <stdint.h>

void oled_init();
void oled_clear();
void oled_print(int x, int y, const char* text);
void oled_update();
void oled_set_status(const char* status);
void oled_set_battery(uint8_t level, bool charging);

#endif // OLED_H
