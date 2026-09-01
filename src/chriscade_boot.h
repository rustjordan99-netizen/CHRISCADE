#pragma once

// CrowPanel status hardware and the CHRISCADE startup screen.
void chriscade_power_init();
void chriscade_boot_screen();

// Called from the running emulator loop. If GP20 is pressed, save the game,
// enter RP2040 dormant mode, and reboot when the button is released/pressed.
void chriscade_power_poll(bool save_game);

// Battery connector voltage measured through the GP29 100k/100k divider.
// Returns zero when no plausible battery voltage has been detected.
unsigned int chriscade_battery_millivolts();
bool chriscade_battery_is_low();

// Lightweight, volume-pot-controlled interface sound used while transferring
// a cartridge after the main boot audio system has shut down.
void chriscade_loading_sound_begin();
void chriscade_loading_sound_set(unsigned int frequency);
void chriscade_loading_sound_end();
// Full-volume, volume-pot-controlled tone path for persistent alarms.
void chriscade_alarm_sound_begin();
void chriscade_alarm_sound_set(unsigned int frequency);
void chriscade_alarm_sound_end();
void chriscade_ui_click(unsigned int frequency);
// Short preview used by the Settings screen. Sound 0 is intentionally silent.
void chriscade_preview_boot_sound(unsigned int sound);
