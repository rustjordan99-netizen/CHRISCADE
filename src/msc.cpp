#if ENABLE_USB_STORAGE_DEVICE

#include <Adafruit_TinyUSB.h>

#include "card_loader.h"

static Adafruit_USBD_MSC usb_msc;

// Callback invoked when received READ10 command.
static int32_t msc_read_cb (uint32_t lba, void* buffer, uint32_t bufsize) {
  auto scope = UseSDPinFunctionScope();

  bool rc = sd.card()->readSectors(lba, (uint8_t*) buffer, bufsize/512);
  return rc ? bufsize : -1;
}

// Callback invoked when received WRITE10 command.
static int32_t msc_write_cb (uint32_t lba, uint8_t* buffer, uint32_t bufsize) {
  auto scope = UseSDPinFunctionScope();

  bool rc = sd.card()->writeSectors(lba, buffer, bufsize/512);
  return rc ? bufsize : -1;
}

// Callback invoked when WRITE10 command is completed (status received and accepted by host).
static void msc_flush_cb (void) {
  auto scope = UseSDPinFunctionScope();

  sd.card()->syncDevice();
}

void initUsbStorageDevice() {
  usb_msc.setID("GB", "SD Card", "1.0");
  usb_msc.setReadWriteCallback(msc_read_cb, msc_write_cb, msc_flush_cb);
  usb_msc.setUnitReady(false);
  usb_msc.begin();

  if (TinyUSBDevice.mounted()) {
    TinyUSBDevice.detach();
    delay(10);
    TinyUSBDevice.attach();
  }

  auto scope = UseSDPinFunctionScope();
  uint32_t block_count = sd.card()->sectorCount();
  usb_msc.setCapacity(block_count, 512);
  usb_msc.setUnitReady(true);
}

#endif
