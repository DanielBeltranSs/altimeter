#ifdef ENABLE_DISPLAY
#include "display_ssd1306.h"
#include <math.h>

static constexpr float M2FT = 3.280839895f;

bool DisplaySSD1306::begin(uint8_t i2cAddr, uint32_t i2cHz) {
  if (_bus) _bus->setClock(i2cHz);
  if(!_oled.begin(SSD1306_SWITCHCAPVCC, i2cAddr, false, false)) return false;
  _oled.clearDisplay();
  _oled.setTextWrap(false);
  _oled.setTextColor(SSD1306_WHITE);
  // <<< Importante: NO dibujar nada aquí y apaga el panel
  _oled.ssd1306_command(SSD1306_DISPLAYOFF);
  return true;
}

void DisplaySSD1306::showAltitude(float alt_m, const char* stateText) {
  const float alt_ft = alt_m * M2FT;

  _oled.clearDisplay();

  // Línea 1: estado
  _oled.setTextSize(1);
  _oled.setCursor(0,0);
  _oled.print(stateText ? stateText : "READY");

  // Líneas 2-3: altitud grande en pies
  int alt_i = (int) lroundf(alt_ft);
  char buf[8]; snprintf(buf, sizeof(buf), "%4d", alt_i);

  _oled.setTextSize(3);
  _oled.setCursor(0,8);
  _oled.print(buf);

  // Unidad "ft"
  _oled.setTextSize(1);
  _oled.setCursor(104, 8);
  _oled.print("ft");

  _oled.display();
}

void DisplaySSD1306::showStatus(const char* line1, const char* line2) {
  _oled.clearDisplay();
  _oled.setTextSize(2);
  _oled.setCursor(0,0);  _oled.print(line1 ? line1 : "");
  _oled.setTextSize(1);
  _oled.setCursor(0,18); _oled.print(line2 ? line2 : "");
  _oled.display();
}

void DisplaySSD1306::powerOn()  { _oled.ssd1306_command(SSD1306_DISPLAYON);  }
void DisplaySSD1306::powerOff() { _oled.ssd1306_command(SSD1306_DISPLAYOFF); }

void DisplaySSD1306::clear() {
  _oled.clearDisplay();
  _oled.display();
}

#endif
