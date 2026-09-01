#pragma once

#include <PCF8574.h>

void initJoypad();
bool readJoypad(uint8_t pin);

void handleJoypad();
void handleSerial();

// Called by the display-owning core after an in-game frame transfer. Keeping
// touch reads on that core prevents the XPT2046 and LCD from colliding on SPI1.
void pollGameplayTouch();
