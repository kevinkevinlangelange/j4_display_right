//******************************************************************************
//       file name:  j4_display_right / src/diag_sd/sd_diag.cpp
//     v0_1 created:  2026-10-13 -KL
//       update log:  2026-10-13 -KL  (microSD bring-up diagnostic)
//           author:  Kevin Lange
//      description:  Finds out WHY the TFT module's microSD will not mount.
//                    Runs with nothing but the XIAO and the TFT module. Never
//                    talks to the panel: it only holds the panel's CS high so
//                    the panel stays off the bus. Every 4 seconds it repeats:
//
//                    1. MISO idle test. With both chip selects high, D9 is read
//                       with the ESP32's pull-up and then its pull-down. If it
//                       follows the pull, nothing is driving it (good). If it
//                       reads LOW both ways, something is holding MISO low
//                       while deselected: the display's own MISO/SDO wire, or
//                       a short.
//                    2. Raw card wake-up at 250 kHz: 80 clocks with CS high,
//                       then CMD0 (go idle) and CMD8 (voltage check), printing
//                       the bytes the card sends back. This says whether the
//                       card is answering at all, before the SD library's own
//                       retries get involved.
//                    3. SD.begin() at 400 kHz, 1 MHz and 4 MHz (the normal
//                       firmware uses 4 MHz), plus a root listing if it mounts.
//
//                    Output is on USB serial at 115200. Use PlatformIO's
//                    "Upload and Monitor" on the diag-sd environment.
//******************************************************************************

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

// Same pins as the normal firmware (platformio.ini build flags).
#define PIN_SCK      7    // D8
#define PIN_MISO     8    // D9
#define PIN_MOSI     9    // D10
#define PIN_SD_CS    1    // D0
#define PIN_TFT_CS   4    // D3, held high the whole time

#define REPEAT_MS    4000

SPIClass sdSpi(FSPI);


// ---- 1. MISO idle test --------------------------------------------------
void misoIdleTest() {
  digitalWrite(PIN_SD_CS, HIGH);
  digitalWrite(PIN_TFT_CS, HIGH);
  pinMode(PIN_MISO, INPUT_PULLUP);
  delay(5);
  int up = digitalRead(PIN_MISO);
  pinMode(PIN_MISO, INPUT_PULLDOWN);
  delay(5);
  int down = digitalRead(PIN_MISO);
  pinMode(PIN_MISO, INPUT);

  Serial.printf("[1] MISO (D9) idle, both CS high: pull-up reads %d, pull-down reads %d\n", up, down);
  if (up == 1 && down == 0)
    Serial.println("    OK: nothing is driving MISO while deselected.");
  else if (up == 1 && down == 1)
    Serial.println("    OK: held high (most modules have a pull-up resistor on SD_MISO).");
  else
    Serial.println("    FAULT: MISO is held LOW while nothing is selected. The display's MISO/SDO\n"
                   "    wire is still connected, or D9 is shorted to GND. Check before anything else.");
}


// ---- 2. Raw card wake-up -------------------------------------------------
uint8_t sdCommand(uint8_t cmd, uint32_t arg, uint8_t crc, uint8_t *extra, int nExtra) {
  sdSpi.transfer(0xFF);
  sdSpi.transfer(0x40 | cmd);
  sdSpi.transfer(arg >> 24);
  sdSpi.transfer(arg >> 16);
  sdSpi.transfer(arg >> 8);
  sdSpi.transfer(arg);
  sdSpi.transfer(crc);
  uint8_t r = 0xFF;
  for (int i = 0; i < 10 && r == 0xFF; i++) r = sdSpi.transfer(0xFF);
  for (int i = 0; i < nExtra; i++) extra[i] = sdSpi.transfer(0xFF);
  return r;
}

void rawWakeTest() {
  sdSpi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);
  sdSpi.beginTransaction(SPISettings(250000, MSBFIRST, SPI_MODE0));

  digitalWrite(PIN_SD_CS, HIGH);
  for (int i = 0; i < 10; i++) sdSpi.transfer(0xFF);   // 80 clocks, CS high

  digitalWrite(PIN_SD_CS, LOW);
  uint8_t r0 = 0xFF;
  for (int tries = 0; tries < 5 && r0 != 0x01; tries++)
    r0 = sdCommand(0, 0, 0x95, NULL, 0);
  uint8_t ocr[4] = { 0, 0, 0, 0 };
  uint8_t r8 = sdCommand(8, 0x1AA, 0x87, ocr, 4);
  digitalWrite(PIN_SD_CS, HIGH);
  sdSpi.transfer(0xFF);

  sdSpi.endTransaction();
  sdSpi.end();

  Serial.printf("[2] Raw CMD0 reply: 0x%02X   CMD8 reply: 0x%02X %02X %02X %02X %02X\n",
                r0, r8, ocr[0], ocr[1], ocr[2], ocr[3]);
  if (r0 == 0x01) {
    Serial.println("    OK: the card is powered, wired and answering.");
    if (r8 == 0x01 && ocr[3] == 0xAA) Serial.println("    OK: SDHC/SDXC-era card, voltage accepted.");
    else if (r8 == 0x05)              Serial.println("    Old SDv1 card. Should still work.");
    else                              Serial.println("    CMD8 odd: possible noise on MOSI/MISO, try shorter wires.");
  } else if (r0 == 0xFF) {
    Serial.println("    FAULT: no reply at all (MISO stays high). The card is not seeing the commands\n"
                   "    or cannot answer: SD_CS / SD_SCK / SD_MOSI / SD_MISO not wired to D0 / D8 / D10 / D9,\n"
                   "    no card in the slot, or the card is not powered (see the J1 note in the README).");
  } else if (r0 == 0x00) {
    Serial.println("    FAULT: all zeros. MISO is being held low: display MISO/SDO still wired, or a short.");
  } else {
    Serial.println("    FAULT: garbled reply. Something is answering but the bits are wrong: a loose or\n"
                   "    swapped wire (MOSI/MISO crossed?), long jumpers, or low card voltage.");
  }
}


// ---- 3. Library mount ----------------------------------------------------
const char *typeName(uint8_t t) {
  switch (t) {
    case CARD_MMC:  return "MMC";
    case CARD_SD:   return "SDSC";
    case CARD_SDHC: return "SDHC/SDXC";
    default:        return "unknown";
  }
}

void mountTest() {
  const uint32_t freqs[3] = { 400000, 1000000, 4000000 };
  for (int i = 0; i < 3; i++) {
    sdSpi.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);
    bool ok = SD.begin(PIN_SD_CS, sdSpi, freqs[i]);
    Serial.printf("[3] SD.begin at %4lu kHz: %s", (unsigned long)(freqs[i] / 1000), ok ? "MOUNTED" : "failed");
    if (ok) {
      Serial.printf(", %s, %llu MB\n", typeName(SD.cardType()), SD.cardSize() >> 20);
      File root = SD.open("/");
      int n = 0;
      for (File f = root.openNextFile(); f && n < 10; f = root.openNextFile(), n++)
        Serial.printf("      %s%s  %u bytes\n", f.name(), f.isDirectory() ? "/" : "", (unsigned)f.size());
      root.close();
      if (n == 0) Serial.println("      (card is empty)");
    } else {
      Serial.println(SD.cardType() == CARD_NONE ? " (no card detected)"
                                                : " (card answered, but no FAT32 filesystem: reformat FAT32)");
    }
    SD.end();
    sdSpi.end();
  }
}


void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 4000) delay(10);

  pinMode(PIN_SD_CS, OUTPUT);
  pinMode(PIN_TFT_CS, OUTPUT);
  digitalWrite(PIN_SD_CS, HIGH);
  digitalWrite(PIN_TFT_CS, HIGH);

  Serial.println();
  Serial.println("j4_display_right microSD diagnostic");
  Serial.println("pins: SD_CS=D0  SCK=D8  MISO=D9  MOSI=D10  (TFT CS=D3 held high)");
}

void loop() {
  Serial.println("------------------------------------------------------------");
  misoIdleTest();
  rawWakeTest();
  mountTest();
  delay(REPEAT_MS);
}
