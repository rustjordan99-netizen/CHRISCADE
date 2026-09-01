#if ENABLE_SDCARD
#include "rom_usb_upload.h"
#include "rom_upload_protocol.h"
#include "card_loader.h"
#include "chriscade_boot.h"
#include "chriscade_settings.h"
#include "input.h"

static void transfer_status(const char* text) {
  const uint16_t panel = chriscade_theme_panel();
  tft.fillRoundRect(18, 137, 284, 30, 10, panel);
  tft.setTextColor(TFT_WHITE, panel);
  tft.drawCentreString(text, 160, 147, 1);
}

void rom_usb_upload(uint8_t* workspace, size_t workspace_size) {
  if (workspace_size < 1536) return;
  char* line = reinterpret_cast<char*>(workspace);
  char* filename = reinterpret_cast<char*>(workspace + 768);
  uint8_t* data = workspace + 1024;
  const uint16_t bg = chriscade_theme_bg();
  const uint16_t accent = chriscade_theme_primary();
  tft.fillScreen(bg);
  tft.setTextColor(accent, bg);
  tft.drawString("CHRISCADE // ADD GAME", 18, 18, 2);
  tft.drawRoundRect(18, 57, 284, 67, 16, accent);
  tft.setTextColor(TFT_WHITE, bg);
  tft.drawCentreString("CONNECT USB TO YOUR PC", 160, 72, 1);
  tft.drawCentreString("RUN  SEND_GAME_USB.cmd", 160, 94, 1);
  tft.setTextColor(chriscade_theme_secondary(), bg);
  tft.drawCentreString("ROM FILES ONLY // EXISTING FILES KEPT", 160, 185, 1);
  tft.drawCentreString("B BACK / CANCEL", 160, 215, 1);
  transfer_status("WAITING FOR ROM SENDER");
  // Discard unrelated console input from before this explicit transfer screen.
  while (Serial.available()) Serial.read();
  size_t line_length = 0;
  bool overflow = false;
  uint32_t last_input = millis();
  while (true) {
    chriscade_power_poll(false); // No output file is open in this idle loop.
    if (!readJoypad(PIN_B)) return;
    if (!Serial.available()) { delay(1); continue; }
    if ((uint32_t)(millis() - last_input) > 10000) { line_length = 0; overflow = false; }
    last_input = millis();
    const char c = Serial.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (!c || line_length >= 767) overflow = true;
      else if (!overflow) line[line_length++] = c;
      continue;
    }
    line[line_length] = '\0';
    line_length = 0;
    if (overflow) { overflow = false; Serial.println("ERR HEADER"); continue; }
    if (!strcmp(line, "HELLO")) { Serial.println("CCREADY"); continue; }
    uint32_t expected_size, expected_crc;
    if (!RomUpload::parse_header(line, expected_size, expected_crc, filename)) {
      Serial.println("ERR NAME_OR_SIZE");
      continue;
    }
    RomUpload::Receiver<FsFile> receiver;
    bool opened = false;
    {
      auto scope = UseSDPinFunctionScope();
      for (unsigned attempt = 0; attempt < 8 && !opened; ++attempt) {
        char temporary[20];
        snprintf(temporary, sizeof(temporary), "CC%08lx.TMP", (unsigned long)(micros() + attempt));
        opened = receiver.begin(filename, temporary, O_RDONLY, O_WRONLY | O_CREAT | O_EXCL);
      }
    }
    if (!opened) {
      Serial.println("ERR EXISTS_OR_SD");
      transfer_status("NAME EXISTS OR SD ERROR // TRY AGAIN");
      continue;
    }
    transfer_status("RECEIVING // KEEP USB CONNECTED");
    Serial.println("READY");
    bool ok = true;
    int last_percent = -1;
    while (ok && receiver.received < expected_size) {
      const size_t wanted = min((uint32_t)RomUpload::chunk_bytes, expected_size - receiver.received);
      size_t got = 0;
      uint32_t last_byte = millis();
      while (got < wanted) {
        if (!readJoypad(PIN_B) || (uint32_t)(millis() - last_byte) > 10000) { ok = false; break; }
        if (Serial.available()) { data[got++] = Serial.read(); last_byte = millis(); }
        else delay(1);
      }
      if (!ok) break;
      {
        auto scope = UseSDPinFunctionScope();
        ok = receiver.write(data, wanted, expected_size);
      }
      if (!ok) break;
      const int percent = (uint64_t)receiver.received * 100 / expected_size;
      if (percent != last_percent) {
        char status[40];
        snprintf(status, sizeof(status), "COPYING ROM // %d%%", percent);
        transfer_status(status);
        last_percent = percent;
        // ADC/GPIO only; don't enter deep sleep with a transfer file open.
        chriscade_battery_millivolts();
      }
      Serial.println("ACK");
    }
    {
      auto scope = UseSDPinFunctionScope();
      if (ok) ok = receiver.commit(filename, expected_size, expected_crc);
      if (!ok) receiver.cancel();
    }
    Serial.println(ok ? "DONE" : "ERR TRANSFER_CANCELLED_OR_VERIFY_FAILED");
    transfer_status(ok ? "GAME ADDED // PRESS B TO RETURN" : "NOT ADDED // PRESS B TO RETURN");
    if (ok) chriscade_ui_click(880);
    // Leave this session closed after any transfer. A host must explicitly
    // reopen Add Game to retry; leftover binary bytes are never commands.
    while (readJoypad(PIN_B)) { chriscade_power_poll(false); delay(5); }
    return;
  }
}
#endif
