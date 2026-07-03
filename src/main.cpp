//******************************************************************************
//       file name:  j4_display_right
//     v0_1 created:  2026-07-02 -- CDT -KL
//     last updated:  2026-07-02 -- CDT
//           author:  Kevin Lange
//      description:  Pot-label display for the Johnny 4 controller (the landscape
//                    display on the RIGHT of the panel). Sits directly above four
//                    potentiometers and acts as an electronic label for them:
//                    the bottom of the screen shows IRIS, COLOR, BRIGHTNESS, and
//                    VOLUME (left to right), each with a live value bar.
//
//                    The four pots are read by a dedicated ADS1115 on this
//                    board's I2C bus (the controller's own two ADS1115s are out
//                    of channels), and the raw readings are streamed to the TTGO
//                    j4_controller over UART as an ASCII line at 25 Hz:
//
//                       "P:<iris>,<color>,<brightness>,<volume>\n"
//
//                    The values are raw ADS1115 counts; the controller runs its
//                    usual processPot() scaling on them. The steady stream also
//                    doubles as this board's heartbeat for the controller's
//                    connection-status screen.
//
//
//      J4_DISPLAY_RIGHT - SeeedStudio XIAO ESP32S3 Pin Connections
//      ------------------------------------------------------------------
//      D0 / GPIO1   TFT backlight (HIGH = on)
//      D1 / GPIO2   TFT RST
//      D2 / GPIO3   TFT DC
//      D3 / GPIO4   TFT CS
//      D4 / GPIO5   I2C SDA -- ADS1115 (addr 0x48, ADDR pin to GND)
//      D5 / GPIO6   I2C SCL -- ADS1115
//      D6 / GPIO43  LINK TX  →  TTGO GPIO26  (TTGO RX)
//      D7 / GPIO44  LINK RX  ←  TTGO GPIO25  (TTGO TX, reserved -- nothing
//                                             is sent this way yet)
//      D8 / GPIO7   TFT SCLK
//      D9 / GPIO8   TFT MISO
//      D10 / GPIO9  TFT MOSI
//      GND          GND (shared with TTGO GND)
//      ------------------------------------------------------------------
//
//      ADS1115 channels (pots left to right beneath the display):
//        A0: IRIS       A1: COLOR       A2: BRIGHTNESS       A3: VOLUME
//      Gain/data-rate/mode match the controller's ADS1115s, so the raw
//      counts scale identically (0 to ~17000 across a 3.3V pot).
//
//
//      DISPLAY LAYOUT  (480 x 320 landscape)
//      ------------------------------------------------------------------
//      +------------------------------------------------+  y=0
//      |                     KEVCO                      |  header (44px)
//      +------------------------------------------------+  green line
//      |                                                |
//      |             (reserved for later)               |
//      |                                                |
//      +------------------------------------------------+  dim line
//      |  [bar]     [bar]      [bar]        [bar]       |  value bars (36px)
//      |  IRIS      COLOR      BRIGHTNESS   VOLUME      |  labels (44px)
//      +------------------------------------------------+  y=320
//      Each label cell is a quarter of the width, centered over its pot.
//      ------------------------------------------------------------------
//******************************************************************************


#include <Arduino.h>
#include <Wire.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <ADS1X15.h>


// -----------------------------------------------------------------------------
//  UART LINK (to the TTGO controller)
// -----------------------------------------------------------------------------
#define LINK_RX    44      // D7 receives from TTGO GPIO25 (reserved, unused)
#define LINK_TX    43      // D6 sends to   TTGO GPIO26
#define LINK_BAUD  115200

#define POT_TX_INTERVAL_MS  40   // 25 Hz, matches the controller's control loop


// -----------------------------------------------------------------------------
//  ADS1115 (dedicated to this board -- the controller's two are full).
//  Same gain/rate/mode as the controller's, so raw counts scale identically.
// -----------------------------------------------------------------------------
ADS1115 ADS(0x48);   // ADDR pin to GND

#define CH_IRIS        0
#define CH_COLOR       1
#define CH_BRIGHTNESS  2
#define CH_VOLUME      3

// Raw pot range for the value bars (matches the controller's processPot() cap:
// a 3.3V pot on a gain-0 ADS1115 tops out around 17000 counts).
#define POT_RAW_MAX  17000


// -----------------------------------------------------------------------------
//  COLOR PALETTE (matches j4_display_left)
// -----------------------------------------------------------------------------
#define C_BG     TFT_BLACK
#define C_GREEN  TFT_GREEN
#define C_DIM    0x4208      // dark grey
#define C_HEADER 0x1082      // very dark blue


// -----------------------------------------------------------------------------
//  LAYOUT  (480 x 320 landscape)
//  If the display appears upside down, change setRotation(1) to setRotation(3).
// -----------------------------------------------------------------------------
#define SCREEN_W  480
#define SCREEN_H  320

#define HDR_Y       0
#define HDR_H      44

#define LBL_H      44                        // label strip at the very bottom
#define LBL_Y      (SCREEN_H - LBL_H)
#define BAR_H      36                        // value bars above the labels
#define BAR_Y      (LBL_Y - BAR_H)
#define CELL_W     (SCREEN_W / 4)            // 120px per pot cell

#define BAR_PAD    18                        // bar inset from the cell edges
#define BAR_INNER  (CELL_W - 2 * BAR_PAD)    // drawable bar width

const char *POT_LABELS[4] = { "IRIS", "COLOR", "BRIGHTNESS", "VOLUME" };


// -----------------------------------------------------------------------------
//  GLOBALS
// -----------------------------------------------------------------------------
TFT_eSPI tft = TFT_eSPI();

int16_t potRaw[4]  = { 0, 0, 0, 0 };
int16_t potBarPx[4] = { -1, -1, -1, -1 };   // last drawn bar width (-1 = force draw)

unsigned long potTx_previousMillis = 0;


// -----------------------------------------------------------------------------
//  DRAWING
// -----------------------------------------------------------------------------

void drawHeader() {
  tft.fillRect(0, HDR_Y, SCREEN_W, HDR_H, C_HEADER);
  tft.setTextColor(C_GREEN, C_HEADER);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("KEVCO", SCREEN_W / 2, HDR_Y + HDR_H / 2, 4);
  tft.setTextDatum(TL_DATUM);
}


void drawLabels() {
  tft.fillRect(0, LBL_Y, SCREEN_W, LBL_H, C_BG);
  tft.setTextDatum(MC_DATUM);
  for (uint8_t i = 0; i < 4; i++) {
    int16_t cx = i * CELL_W + CELL_W / 2;
    tft.setTextColor(C_GREEN, C_BG);
    tft.drawString(POT_LABELS[i], cx, LBL_Y + LBL_H / 2, 4);
    if (i > 0) tft.drawFastVLine(i * CELL_W, LBL_Y + 6, LBL_H - 12, C_DIM);
  }
  tft.setTextDatum(TL_DATUM);
}


// One pot's live value bar: green fill proportional to the raw reading.
void drawBar(uint8_t i) {
  int16_t px = (int32_t)constrain(potRaw[i], 0, POT_RAW_MAX) * BAR_INNER / POT_RAW_MAX;
  if (px == potBarPx[i]) return;   // unchanged -- skip the redraw
  potBarPx[i] = px;

  int16_t x = i * CELL_W + BAR_PAD;
  tft.drawRect(x, BAR_Y + 8, BAR_INNER, BAR_H - 16, C_DIM);
  tft.fillRect(x + 2, BAR_Y + 10, px > BAR_INNER - 4 ? BAR_INNER - 4 : px,
               BAR_H - 20, C_GREEN);
  tft.fillRect(x + 2 + px, BAR_Y + 10, BAR_INNER - 4 - px, BAR_H - 20, C_BG);
}


void drawScreen() {
  tft.fillScreen(C_BG);
  drawHeader();
  tft.drawFastHLine(0, HDR_H, SCREEN_W, C_GREEN);
  tft.drawFastHLine(0, BAR_Y - 2, SCREEN_W, C_DIM);
  drawLabels();
  for (uint8_t i = 0; i < 4; i++) drawBar(i);
}


// -----------------------------------------------------------------------------
//  SETUP
// -----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial1.begin(LINK_BAUD, SERIAL_8N1, LINK_RX, LINK_TX);

  // ADS1115 on the XIAO's native I2C pins (D4/D5 = GPIO5/6)
  Wire.begin();
  ADS.begin();
  delay(10);
  ADS.setGain(0);      // +/-6.144V -- same as the controller's ADS1115s
  ADS.setDataRate(7);  // fast
  ADS.setMode(1);      // single-shot

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  tft.init();
  tft.setRotation(1);    // landscape 480x320; change to 3 if upside down
  tft.fillScreen(C_BG);
  tft.setSwapBytes(true);

  drawScreen();
}


// -----------------------------------------------------------------------------
//  MAIN LOOP -- read the four pots, refresh the bars, stream to the TTGO.
// -----------------------------------------------------------------------------
void loop() {
  unsigned long now = millis();

  if (now - potTx_previousMillis >= POT_TX_INTERVAL_MS) {
    potTx_previousMillis = now;

    potRaw[CH_IRIS]       = ADS.readADC(CH_IRIS);
    potRaw[CH_COLOR]      = ADS.readADC(CH_COLOR);
    potRaw[CH_BRIGHTNESS] = ADS.readADC(CH_BRIGHTNESS);
    potRaw[CH_VOLUME]     = ADS.readADC(CH_VOLUME);

    // Raw counts upstream; the controller runs processPot() on them.
    Serial1.printf("P:%d,%d,%d,%d\n",
                   potRaw[CH_IRIS], potRaw[CH_COLOR],
                   potRaw[CH_BRIGHTNESS], potRaw[CH_VOLUME]);

    for (uint8_t i = 0; i < 4; i++) drawBar(i);
  }
}
