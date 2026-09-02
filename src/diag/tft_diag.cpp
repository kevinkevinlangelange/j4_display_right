//******************************************************************************
//       file name:  tft_diag
//     v0_1 created:  2026-09-01 -- CDT -KL
//     last updated:  2026-09-01 -- CDT
//           author:  Kevin Lange
//      description:  Standalone TFT bring-up diagnostic for j4_display_right.
//                    Built by the diag-* environments in platformio.ini, which
//                    exclude the normal src/main.cpp. Nothing on this board's
//                    UART link or ADS1115 is touched, so it can run with the
//                    TTGO and the pots unplugged.
//
//                    Purpose: the panel is blank while the rest of the board
//                    works. TFT_eSPI writes are blind (no ack, no readback in
//                    the init path), so a clean tft.init() proves only that the
//                    ESP32 shifted bytes out, not that the panel took them.
//                    This sketch separates the questions:
//
//                      1. Is the firmware running at all?  The XIAO's on-board
//                         user LED (GPIO21, active LOW) blinks every 500 ms and
//                         the USB CDC serial port narrates every step. Both are
//                         independent of the TFT and of the backlight, which on
//                         the current wiring is strapped to 3.3V and therefore
//                         says nothing about firmware state.
//
//                      2. Which controller is on the glass?  These red 480x320
//                         modules ship as ILI9488, ILI9486 or ST7796 with
//                         identical silkscreen and identical pinout. Flash
//                         diag-ili9488, diag-st7796 and diag-ili9486 in turn;
//                         the one that paints is the right driver. The sketch
//                         also attempts an ID readback over MISO, which names
//                         the controller outright when SDO is wired and driven.
//
//                      3. Does it paint at all before it paints correctly?
//                         Solid full-screen floods run first, then a colour-bar
//                         and text frame. A blank screen through the floods is
//                         a different fault from floods that work with wrong
//                         colours (see the serial hints at the end of a cycle).
//
//                    Runs at SPI_FREQUENCY from platformio.ini (10 MHz in the
//                    diag envs, down from the 27 MHz the normal firmware uses)
//                    to take marginal wiring out of the picture.
//******************************************************************************

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>

TFT_eSPI tft = TFT_eSPI();

// XIAO ESP32S3 on-board user LED. Active LOW, wired to GPIO21, and completely
// independent of the TFT backlight pin.
#ifndef LED_BUILTIN
  #define LED_BUILTIN 21
#endif

static const uint32_t HEARTBEAT_MS = 500;    // on-board LED toggle
static const uint32_t STEP_MS      = 1500;   // dwell per diagnostic step

// Which driver this build selected. TFT_eSPI takes exactly one.
#if   defined(ILI9488_DRIVER)
  static const char *DRIVER_NAME = "ILI9488";
#elif defined(ILI9486_DRIVER)
  static const char *DRIVER_NAME = "ILI9486";
#elif defined(ST7796_DRIVER)
  static const char *DRIVER_NAME = "ST7796";
#elif defined(ILI9341_DRIVER)
  static const char *DRIVER_NAME = "ILI9341";
#else
  static const char *DRIVER_NAME = "UNKNOWN";
#endif

struct Flood {
  uint16_t    colour;
  const char *name;
};

// Primaries first: red/green/blue tell colour-order faults apart at a glance.
static const Flood FLOODS[] = {
  { TFT_RED,   "RED"   },
  { TFT_GREEN, "GREEN" },
  { TFT_BLUE,  "BLUE"  },
  { TFT_WHITE, "WHITE" },
  { TFT_BLACK, "BLACK" },
};
static const uint8_t FLOOD_COUNT = sizeof(FLOODS) / sizeof(FLOODS[0]);

static uint32_t heartbeat_previousMillis = 0;
static uint32_t step_previousMillis      = 0;
static uint8_t  stepIndex                = 0;
static uint16_t cycleCount               = 0;
static bool     ledState                 = false;


// -----------------------------------------------------------------------------
//  Report the compiled-in pin map, so the serial log is self-contained when it
//  gets pasted somewhere. These are the build flags from platformio.ini, not a
//  reading of the physical wiring.
// -----------------------------------------------------------------------------
static void printConfig() {
  Serial.println();
  Serial.println(F("=========================================="));
  Serial.println(F("  j4_display_right -- TFT bring-up diag"));
  Serial.println(F("=========================================="));
  Serial.print  (F("driver      : ")); Serial.println(DRIVER_NAME);
  Serial.print  (F("panel       : ")); Serial.print(TFT_WIDTH);
  Serial.print  (F(" x "));            Serial.println(TFT_HEIGHT);
  Serial.print  (F("SPI write Hz: ")); Serial.println(SPI_FREQUENCY);
#ifdef SPI_READ_FREQUENCY
  Serial.print  (F("SPI read Hz : ")); Serial.println(SPI_READ_FREQUENCY);
#endif
  Serial.println(F("--- pins (GPIO, from build flags) --------"));
  Serial.print  (F("  SCLK  D8  GPIO")); Serial.println(TFT_SCLK);
  Serial.print  (F("  MOSI  D10 GPIO")); Serial.println(TFT_MOSI);
#ifdef TFT_MISO
  Serial.print  (F("  MISO  D9  GPIO")); Serial.println(TFT_MISO);
#else
  Serial.println(F("  MISO  not defined (ID readback unavailable)"));
#endif
  Serial.print  (F("  CS    D3  GPIO")); Serial.println(TFT_CS);
  Serial.print  (F("  DC    D2  GPIO")); Serial.println(TFT_DC);
  Serial.print  (F("  RST   D1  GPIO")); Serial.println(TFT_RST);
  Serial.print  (F("  BL    D0  GPIO")); Serial.println(TFT_BL);
  Serial.println(F("  NOTE: with the display LED pin strapped to 3.3V,"));
  Serial.println(F("        the backlight is on regardless of GPIO"));
  Serial.println(F("        state and proves nothing about firmware."));
  Serial.println(F("------------------------------------------"));
}


// -----------------------------------------------------------------------------
//  Explicit hardware reset. TFT_eSPI pulses RST inside init(), but doing it here
//  first makes the sequence visible in the log and rules out a panel left in a
//  bad state by a previous flash.
// -----------------------------------------------------------------------------
static void hardReset() {
  Serial.println(F("[1] pulsing RST low 20ms, then 150ms settle"));
  pinMode(TFT_RST, OUTPUT);
  digitalWrite(TFT_RST, HIGH);
  delay(10);
  digitalWrite(TFT_RST, LOW);
  delay(20);
  digitalWrite(TFT_RST, HIGH);
  delay(150);
}


// -----------------------------------------------------------------------------
//  Read the controller's ID registers over MISO. This is the one test that can
//  name the chip outright instead of inferring it from which driver paints.
//
//  All 0x00 or all 0xFF means no usable readback: either SDO is unwired, or the
//  module does not drive SDO at all (common on these red boards, and not in
//  itself a fault). The touch controller has its own T_* pins on this header,
//  so it is isolated unless those are wired in common; on modules where its
//  DOUT is commoned with SDO internally, a floating T_CS can also let it
//  contend, making a garbage readback inconclusive. In any of those cases,
//  fall back to which diag-* build paints.
// -----------------------------------------------------------------------------
static void readPanelId() {
  Serial.println(F("[3] ID readback over MISO"));

#ifndef TFT_MISO
  Serial.println(F("    skipped, TFT_MISO not defined"));
  return;
#else
  uint8_t rddid[4];
  uint8_t rddst[4];
  bool    sawSomething = false;

  for (uint8_t i = 0; i < 4; i++) {
    rddid[i] = tft.readcommand8(0x04, i);   // RDDID  manufacturer / version / driver
    rddst[i] = tft.readcommand8(0x09, i);   // RDDST  display status
    if (rddid[i] != 0x00 && rddid[i] != 0xFF) sawSomething = true;
    if (rddst[i] != 0x00 && rddst[i] != 0xFF) sawSomething = true;
  }

  Serial.print(F("    RDDID(0x04): "));
  for (uint8_t i = 0; i < 4; i++) {
    Serial.print(F("0x")); Serial.print(rddid[i], HEX); Serial.print(' ');
  }
  Serial.println();

  Serial.print(F("    RDDST(0x09): "));
  for (uint8_t i = 0; i < 4; i++) {
    Serial.print(F("0x")); Serial.print(rddst[i], HEX); Serial.print(' ');
  }
  Serial.println();

  if (!sawSomething) {
    Serial.println(F("    all 0x00/0xFF -- no readback. Either SDO is not"));
    Serial.println(F("    wired, or this module does not drive SDO at all."));
    Serial.println(F("    Not a fault on its own; identify the controller"));
    Serial.println(F("    by which diag build paints."));
  } else {
    Serial.println(F("    RDDID byte 3 is the driver code on most panels:"));
    Serial.println(F("      0x88 -> ILI9488   0x86 -> ILI9486   0x96 -> ST7796"));
  }
#endif
}


// -----------------------------------------------------------------------------
//  Colour bars plus text. Reached only if the floods worked, and answers the
//  follow-up questions: is the colour order right, is the geometry right, and
//  is text legible at the size the real firmware uses.
// -----------------------------------------------------------------------------
static void drawPattern() {
  const int16_t w = tft.width();
  const int16_t h = tft.height();

  const uint16_t bars[8]      = { TFT_RED, TFT_GREEN, TFT_BLUE,  TFT_CYAN,
                                  TFT_MAGENTA, TFT_YELLOW, TFT_WHITE, TFT_BLACK };
  const char    *barNames[8]  = { "R", "G", "B", "C", "M", "Y", "W", "K" };

  tft.fillScreen(TFT_BLACK);

  const int16_t barW = w / 8;
  const int16_t barY = h / 2;
  const int16_t barH = h - barY - 40;

  for (uint8_t i = 0; i < 8; i++) {
    tft.fillRect(i * barW, barY, barW, barH, bars[i]);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(barNames[i], i * barW + barW / 2, h - 20, 2);
  }

  // Frame the full panel: if an edge is missing, width/height or rotation is
  // wrong for this controller.
  tft.drawRect(0, 0, w, h, TFT_WHITE);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.drawString(DRIVER_NAME, w / 2, 50, 4);

  char dims[32];
  snprintf(dims, sizeof(dims), "%d x %d", w, h);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(dims, w / 2, 100, 4);
  tft.drawString("KEVCO", w / 2, 150, 4);

  tft.setTextDatum(TL_DATUM);
}


// -----------------------------------------------------------------------------
//  What the results mean. Printed once per completed cycle so the hints are
//  always next to the run they describe.
// -----------------------------------------------------------------------------
static void printHints() {
  Serial.println();
  Serial.println(F("--- reading the result -------------------"));
  Serial.print  (F("driver under test: ")); Serial.println(DRIVER_NAME);
  Serial.println(F("  nothing at all, any driver -> not a driver"));
  Serial.println(F("     mismatch. Check CS/DC/RST/SCLK/MOSI"));
  Serial.println(F("     continuity and the module supply."));
  Serial.println(F("  floods paint -> this driver is correct."));
  Serial.println(F("  colours swapped (RED floods blue) -> right"));
  Serial.println(F("     driver, wrong colour order. Add"));
  Serial.println(F("     -DTFT_RGB_ORDER=TFT_BGR (or TFT_RGB)."));
  Serial.println(F("  inverted / washed out -> try"));
  Serial.println(F("     -DTFT_INVERSION_ON or -DTFT_INVERSION_OFF."));
  Serial.println(F("  intermittent, tearing, garbage -> raise"));
  Serial.println(F("     SPI_FREQUENCY back gradually; 10 MHz here,"));
  Serial.println(F("     27 MHz in the normal firmware."));
  Serial.println(F("------------------------------------------"));
}


// -----------------------------------------------------------------------------
//  SETUP
// -----------------------------------------------------------------------------
void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);   // active LOW, so HIGH is off

  Serial.begin(115200);
  // USB CDC enumerates after boot. Wait briefly so the banner is not lost, but
  // never block: the board must run headless with no host attached.
  uint32_t waitStart = millis();
  while (!Serial && millis() - waitStart < 3000) delay(10);

  printConfig();

  // Drive the backlight pin anyway, so this build still works unchanged once
  // the display LED pin is moved from 3.3V to D0.
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);

  hardReset();

  Serial.println(F("[2] tft.init() + setRotation(1)"));
  tft.init();
  tft.setRotation(1);
  Serial.print(F("    reported size: "));
  Serial.print(tft.width()); Serial.print(F(" x ")); Serial.println(tft.height());

  readPanelId();

  Serial.println(F("[4] starting flood cycle, watch the panel"));
  step_previousMillis = millis();
}


// -----------------------------------------------------------------------------
//  MAIN LOOP -- heartbeat plus one diagnostic step every STEP_MS.
// -----------------------------------------------------------------------------
void loop() {
  uint32_t now = millis();

  // Firmware heartbeat, independent of the TFT and of the backlight.
  if (now - heartbeat_previousMillis >= HEARTBEAT_MS) {
    heartbeat_previousMillis = now;
    ledState = !ledState;
    digitalWrite(LED_BUILTIN, ledState ? LOW : HIGH);
  }

  if (now - step_previousMillis >= STEP_MS) {
    step_previousMillis = now;

    if (stepIndex < FLOOD_COUNT) {
      Serial.print(F("    flood: "));
      Serial.println(FLOODS[stepIndex].name);
      tft.fillScreen(FLOODS[stepIndex].colour);
    } else {
      Serial.println(F("    pattern: colour bars + text"));
      drawPattern();
      printHints();
      cycleCount++;
      Serial.print(F("cycle ")); Serial.print(cycleCount);
      Serial.println(F(" complete, repeating"));
      stepIndex = 0;
      return;
    }

    stepIndex++;
  }
}
