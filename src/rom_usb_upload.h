#pragma once
#include <stddef.h>
#include <stdint.h>

// Foreground, ROM-only USB serial transfer. Never exposes the SD as a drive.
// Reuses the loader workspace; call only from the library before gameplay.
void rom_usb_upload(uint8_t* workspace, size_t workspace_size);
