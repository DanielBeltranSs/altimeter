#include "datetime_module.h"
#include <Arduino.h>
#include <Preferences.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <time.h>

#include "config.h"
#if USE_DS3231
  #include "rtc_ds3231.h"
  #ifndef DS3231_STORES_UTC
    #define DS3231_STORES_UTC 1   // guardamos/leeremos UTC del RTC
  #endif
#endif
#ifndef RTC_REQUIRE_DS3231
  #define RTC_REQUIRE_DS3231 0    // 1 = si no hay DS3231, no hay hora válida
#endif

// ---- forward decl. portátil del contador RTC en µs
extern "C" uint64_t esp_rtc_get_time_us(void);

#if __has_include("esp_idf_version.h")
  #include "esp_idf_version.h"
#else
  #define ESP_IDF_VERSION_VAL(major, minor, patch) (0)
  #define ESP_IDF_VERSION 0
#endif

// ========= Persistencia (NVS) =========
static Preferences s_prefs;
static const char* NVS_NS  = "timebase";
static const char* NVS_KEY = "tbv2";

// ========= Estado =========
typedef struct {
  int64_t epoch_s_at_sync;   // epoch (UTC) de referencia
  int64_t mono_us_at_sync;   // esp_timer_get_time() cuando se guardó
  int32_t tz_minutes;        // zona horaria (minutos)
  uint8_t valid;             // 1 = base válida
} __attribute__((packed)) TB;

static TB s_tb = {0,0,-180,0};

// ====== Persistentes en deep sleep ======
RTC_DATA_ATTR static uint64_t s_rtc_before_ds_us = 0;
RTC_DATA_ATTR static uint32_t s_rtc_magic        = 0;
static constexpr uint32_t RTC_MAGIC = 0x51C0FFEE;

// ========= Calendario (aux) =========
static int daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y-399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153*(m + (m > 2 ? -3 : 9)) + 2)/5 + d-1;
  const unsigned doe = yoe * 365 + yoe/4 - yoe/100 + doy;
  return era * 146097 + (int)doe - 719468;
}
static int64_t makeEpochUTC(int y,int M,int d,int h,int m,int s){
  const int days = daysFromCivil(y, (unsigned)M, (unsigned)d);
  return (int64_t)days*86400 + (int64_t)h*3600 + (int64_t)m*60 + (int64_t)s;
}

// ========= NVS =========
static void saveTB() {
  s_prefs.begin(NVS_NS, false);
  s_prefs.putBytes(NVS_KEY, &s_tb, sizeof(s_tb));
  s_prefs.end();
}
static bool loadTB() {
  s_prefs.begin(NVS_NS, true);
  size_t got = s_prefs.getBytes(NVS_KEY, &s_tb, sizeof(s_tb));
  s_prefs.end();
  if (got == sizeof(s_tb) && s_tb.valid == 1) return true;
  s_tb.tz_minutes = (s_tb.tz_minutes==0 ? -180 : s_tb.tz_minutes);
  s_tb.valid = 0;
  return false;
}

// ==== prototipo para conversión usada más abajo
static bool epochToLocalYMDHMS(uint32_t ts, int tz_min,
                               int &Y, int &M_, int &D,
                               int &h, int &m, int &s);

// ================== DS3231 como autoridad ==================
#if USE_DS3231
static bool     s_ds_present = false;
static uint32_t s_ds_cache_epoch = 0;   // epoch (UTC) en s
static uint32_t s_ds_cache_ms    = 0;   // instante de cache
static const uint16_t DS_CACHE_MS = 200;

// Lee del DS3231 con cache simple para no saturar I2C (≈5 lecturas/seg máx)
static bool ds3231_read_epoch_cached(uint32_t &out_epoch){
  uint32_t now_ms = millis();
  if (s_ds_present && (int32_t)(now_ms - s_ds_cache_ms) >= (int32_t)DS_CACHE_MS) {
    int Y,M,D,h,m,s;
    if (ds3231_read_ymdhms(Y,M,D,h,m,s)) {
      s_ds_cache_epoch = (uint32_t)makeEpochUTC(Y,M,D,h,m,s);
      s_ds_cache_ms = now_ms;
    } else {
      // lectura falló: no actualizamos cache_ms para reintentar pronto
      return false;
    }
  }
  if (!s_ds_present) return false;
  if (s_ds_cache_epoch == 0) {
    // Aún sin cache => intenta una lectura directa
    int Y,M,D,h,m,s;
    if (!ds3231_read_ymdhms(Y,M,D,h,m,s)) return false;
    s_ds_cache_epoch = (uint32_t)makeEpochUTC(Y,M,D,h,m,s);
    s_ds_cache_ms = now_ms;
  }
  out_epoch = s_ds_cache_epoch + ((millis() - s_ds_cache_ms) / 1000); // aproxima tick
  return true;
}
#endif

// ========= API pública =========
void datetimeInit() {
  loadTB();

  // Deep-sleep delta (solo para respaldo interno si no hay DS)
  uint64_t ds_us = 0;
  if (s_rtc_magic == RTC_MAGIC && s_rtc_before_ds_us != 0) {
    const uint64_t now_rtc = esp_rtc_get_time_us();
    if (now_rtc > s_rtc_before_ds_us) ds_us = now_rtc - s_rtc_before_ds_us;
    s_rtc_magic = 0;
  }

#if USE_DS3231
  s_ds_present = ds3231_present();
  if (s_ds_present) {
    // El DS3231 manda: inicializa cache y base interna desde el RTC
    int Y,M,D,h,mi,ss;
    if (ds3231_read_ymdhms(Y,M,D,h,mi,ss)) {
      const int64_t rtc_epoch = makeEpochUTC(Y,M,D,h,mi,ss);
      s_ds_cache_epoch = (uint32_t)rtc_epoch;
      s_ds_cache_ms    = millis();
      s_tb.epoch_s_at_sync = rtc_epoch;
      s_tb.mono_us_at_sync = esp_timer_get_time();
      s_tb.valid = 1;
      saveTB();
      Serial.println("[RTC] DS3231 detectado: autoridad de tiempo.");
      return; // listo
    } else {
      Serial.println("[RTC] DS3231 detectado pero lectura inválida; intentando respaldo interno.");
      // caemos a respaldo interno si se permite
    }
  } else {
    Serial.println("[RTC] DS3231 no detectado.");
    if (RTC_REQUIRE_DS3231) {
      s_tb.valid = 0; saveTB();
      return;
    }
  }
#endif

  // --- Respaldo interno (solo si no se exige DS)
  if (!RTC_REQUIRE_DS3231) {
    if (s_tb.valid == 1) {
      if (ds_us > 0) s_tb.epoch_s_at_sync += (int64_t)(ds_us / 1000000ULL);
      s_tb.mono_us_at_sync = esp_timer_get_time();
      saveTB();
    }
  }
}

void datetimeSetManual(int year, int month, int day,
                       int hour, int minute, int second,
                       int tz_minutes) {
  // Calcula epoch UTC desde fecha local + tz
  const int64_t epoch_utc = makeEpochUTC(year, month, day, hour, minute, second)
                          - (int64_t)tz_minutes*60;

#if USE_DS3231
  if (s_ds_present) {
    int Y,M,D,h,m,s;
    // Convertimos epoch UTC a campos y lo escribimos al DS3231
    if (epochToLocalYMDHMS((uint32_t)epoch_utc, /*tz=*/0, Y,M,D,h,m,s)) {
      if (ds3231_write_ymdhms(Y,M,D,h,m,s)) {
        s_ds_cache_epoch = (uint32_t)epoch_utc;
        s_ds_cache_ms    = millis();
        Serial.println("[RTC] DS3231 programado (UTC).");
      } else {
        Serial.println("[RTC] Error programando DS3231.");
      }
    }
  }
#endif

  // Siempre reflejamos en la base (para UI y respaldo)
  s_tb.epoch_s_at_sync = epoch_utc;
  s_tb.mono_us_at_sync = esp_timer_get_time();
  s_tb.tz_minutes      = tz_minutes;
  s_tb.valid           = 1;
  saveTB();
}

int64_t datetimeNowEpoch() {
#if USE_DS3231
  if (s_ds_present) {
    uint32_t e;
    if (ds3231_read_epoch_cached(e)) return (int64_t)e;
    if (RTC_REQUIRE_DS3231) return -1; // autoridad obligatoria
    // si falla lectura puntual, caeremos a respaldo
  }
#endif
  // Respaldo interno
  if (s_tb.valid != 1) return -1;
  const int64_t now_us = esp_timer_get_time();
  int64_t dt_us  = now_us - s_tb.mono_us_at_sync;
  if (dt_us < 0) dt_us = 0;
  return s_tb.epoch_s_at_sync + (dt_us / 1000000LL);
}

int32_t datetimeGetTZMinutes(){ return s_tb.tz_minutes; }
void    datetimeSetTZMinutes(int32_t tz){ s_tb.tz_minutes = tz; saveTB(); }

static void format2(char* p, int v) { p[0]='0'+((v/10)%10); p[1]='0'+(v%10); }

// ==== Formateadores UI ====
void datetimeFormatHHMM(char* buf, size_t n) {
  if (n < 6) return;
  int64_t epoch = datetimeNowEpoch();
  if (epoch < 0) { snprintf(buf, n, "--:--"); return; }
  epoch += (int64_t)s_tb.tz_minutes * 60; // local
  const int min = (int)((epoch/60) % 60);
  const int hr  = (int)((epoch/3600) % 24);
  buf[2]=':'; buf[5]='\0';
  format2(buf+0, hr);
  format2(buf+3, min);
}

void datetimeFormatYMD(char* buf, size_t n) {
  if (n < 11) return;
  int64_t epoch = datetimeNowEpoch();
  if (epoch < 0) { snprintf(buf, n, "----------"); return; }
  epoch += (int64_t)s_tb.tz_minutes * 60; // local
  int64_t z = epoch / 86400; z += 719468;
  int era = (z >= 0 ? z : z - 146096) / 146097;
  int doe = (int)(z - era * 146097);
  int yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
  int y = yoe + era * 400;
  int doy = doe - (365*yoe + yoe/4 - yoe/100);
  int mp = (5*doy + 2)/153;
  int d = doy - (153*mp+2)/5 + 1;
  int m = mp + (mp < 10 ? 3 : -9);
  y += (m <= 2);
  snprintf(buf, n, "%04d-%02d-%02d", y, m, d);
}

void datetimeOnBeforeDeepSleep(uint64_t /*planned_sleep_us*/) {
  // Aunque el DS sea autoridad, dejamos respaldo interno consistente
  s_rtc_before_ds_us = esp_rtc_get_time_us();
  s_rtc_magic        = RTC_MAGIC;

  if (s_tb.valid == 1) {
    const int64_t now_us = esp_timer_get_time();
    const int64_t dt_us  = now_us - s_tb.mono_us_at_sync;
    if (dt_us > 0) {
      s_tb.epoch_s_at_sync += (dt_us / 1000000LL);
      s_tb.mono_us_at_sync  = now_us;
    }
    saveTB();
  }
}

// ====== UI de configuración (DD/MM/YY y HH:MM) ======
#include <U8g2lib.h>
#include "config.h"

extern int idioma;               // 0=ES, 1=EN
#ifndef LANG_ES
#define LANG_ES 0
#define LANG_EN 1
#endif
static inline const char* L(const char* es, const char* en){
  return (idioma == LANG_ES) ? es : en;
}

static bool s_menuActive = false;

enum FieldType : uint8_t {
  FT_DAY=0, FT_MONTH=1, FT_YEAR=2, FT_HOUR=3, FT_MIN=4, FT_SAVE=5, FT_CANCEL=6
};

static uint8_t s_field = 0;
static int y=2025, M=9, d=26, h=12, mi=0;

extern U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;
static void fmt2(char* b, int v) { b[0]='0'+((v/10)%10); b[1]='0'+(v%10); b[2]='\0'; }

bool datetimeMenuActive(){ return s_menuActive; }

// --- Anti-flanco del submenú ---
static bool     g_dt_firstFrame  = true;
static uint32_t g_dt_blockUntil  = 0;
static bool     g_dt_primedPrev  = false;

void datetimeMenuOpen() {
  s_menuActive = true;
  int64_t epoch = datetimeNowEpoch();
  if (epoch > 0) {
    epoch += (int64_t)datetimeGetTZMinutes()*60;
    int64_t z = epoch/86400 + 719468;
    int era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = (int)(z - era * 146097);
    int yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    y = yoe + era * 400;
    int doy = doe - (365*yoe + yoe/4 - yoe/100);
    int mp = (5*doy + 2)/153;
    d = doy - (153*mp+2)/5 + 1;
    M = mp + (mp < 10 ? 3 : -9);
    y += (M <= 2);
    h  = (int)((epoch/3600)%24);
    mi = (int)((epoch/60)%60);
  } else {
    y=2025; M=9; d=26; h=12; mi=0;
  }
  s_field = 0;

  g_dt_firstFrame = true;
  g_dt_blockUntil = millis() + 220;
  g_dt_primedPrev = false;
}

void datetimeMenuClose(){ 
  s_menuActive=false; 
  g_dt_firstFrame = true;
  g_dt_blockUntil = 0;
  g_dt_primedPrev = false;
}

void datetimeMenuDrawAndHandle() {
  if (!s_menuActive) return;

  if (g_dt_firstFrame) {
    if (g_dt_blockUntil == 0) g_dt_blockUntil = millis() + 220;
    g_dt_firstFrame = false;
  }

  // Dibujo
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr);

  u8g2.drawStr(10, 12, "DD  /  MM  /  YY");

  const int xSlots[3] = {10, 50, 90};
  const int yDate = 28;

  char sDD[3], sMM[3], sYY[3]; fmt2(sDD, d); fmt2(sMM, M); fmt2(sYY, y % 100);

  u8g2.setCursor(xSlots[0], yDate); u8g2.print(sDD);
  u8g2.setCursor(xSlots[0]+20, yDate); u8g2.print(" / ");
  u8g2.setCursor(xSlots[1], yDate); u8g2.print(sMM);
  u8g2.setCursor(xSlots[1]+20, yDate); u8g2.print(" / ");
  u8g2.setCursor(xSlots[2], yDate); u8g2.print(sYY);

  const int ARW = 6;
  if (s_field <= FT_YEAR) {
    int slotIdx = (s_field == FT_DAY ? 0 : (s_field == FT_MONTH ? 1 : 2));
    int x_arrow_date = xSlots[slotIdx] - ARW; if (x_arrow_date < 0) x_arrow_date = 0;
    u8g2.drawStr(x_arrow_date, yDate, ">");
  }

  char HH[3], MI[3]; fmt2(HH, h); fmt2(MI, mi);
  const int yTime = 40;

  u8g2.setCursor(10, yTime);  u8g2.print(HH);
  u8g2.setCursor(30, yTime);  u8g2.print(":");
  u8g2.setCursor(50, yTime);  u8g2.print(MI);

  if (s_field==FT_HOUR || s_field==FT_MIN) {
    int x_arrow_time = (s_field==FT_HOUR ? 10 : 50) - ARW;
    if (x_arrow_time < 0) x_arrow_time = 0;
    u8g2.drawStr(x_arrow_time, yTime, ">");
  }

  const int yAcciones   = 55;
  const int xGuardar    = 12;
  const int xCancelar   = 78;

  u8g2.setCursor(xGuardar,  yAcciones);
  u8g2.print(L("Guardar","Save"));
  u8g2.setCursor(xCancelar, yAcciones);
  u8g2.print(L("Cancelar","Cancel"));

  if (s_field==FT_SAVE) {
    int x_arrow = xGuardar - ARW; if (x_arrow < 0) x_arrow = 0;
    u8g2.drawStr(x_arrow, yAcciones, ">");
  } else if (s_field==FT_CANCEL) {
    int x_arrow = xCancelar - ARW; if (x_arrow < 0) x_arrow = 0;
    u8g2.drawStr(x_arrow, yAcciones, ">");
  }

  u8g2.sendBuffer();

  // Entradas
  const bool altDown  = (digitalRead(BUTTON_ALTITUDE) == HIGH);
  const bool okDown   = (digitalRead(BUTTON_OLED)     == HIGH);
  const bool menuDown = (digitalRead(BUTTON_MENU)     == HIGH);

  static bool altPrev=false, okPrev=false, menuPrev=false;
  static uint32_t lastAltEdge=0, lastOkEdge=0, lastMenuEdge=0;
  static const uint16_t EDGE_DEBOUNCE_MS = 40;

  if (!g_dt_primedPrev) {
    altPrev = altDown; okPrev = okDown; menuPrev = menuDown;
    g_dt_primedPrev = true;
  }

  const bool altRiseRaw  = (altDown  && !altPrev);
  const bool okRiseRaw   = (okDown   && !okPrev);
  const bool menuRiseRaw = (menuDown && !menuPrev);

  uint32_t now = millis();
  const bool altRise  = altRiseRaw  && (now - lastAltEdge  > EDGE_DEBOUNCE_MS);
  const bool okRise   = okRiseRaw   && (now - lastOkEdge   > EDGE_DEBOUNCE_MS);
  const bool menuRise = menuRiseRaw && (now - lastMenuEdge > EDGE_DEBOUNCE_MS);

  if (altRise)  lastAltEdge  = now;
  if (okRise)   lastOkEdge   = now;
  if (menuRise) lastMenuEdge = now;

  altPrev  = altDown;
  okPrev   = okDown;
  menuPrev = menuDown;

  if ((int32_t)(millis() - g_dt_blockUntil) < 0) return;

  if (menuRise) { s_field = (s_field + 1) % 7; return; }

  auto dec_wrap = [](int &v, int lo, int hi){ v = (v > lo ? v-1 : hi); };
  auto inc_wrap = [](int &v, int lo, int hi){ v = (v < hi ? v+1 : lo); };

  static uint32_t altHoldStart=0, okHoldStart=0;
  static uint32_t altLastRpt=0,  okLastRpt=0;
  static const uint16_t HOLD_REPEAT_DELAY_MS = 350;
  static const uint16_t REPEAT_MS = 120;

  auto handle_dec = [&](bool){
    switch (s_field) {
      case FT_DAY:   dec_wrap(d , 1 , 31); break;
      case FT_MONTH: dec_wrap(M , 1 , 12); break;
      case FT_YEAR:  dec_wrap(y , 2000, 2099); break;
      case FT_HOUR:  dec_wrap(h , 0 , 23); break;
      case FT_MIN:   dec_wrap(mi, 0 , 59); break;
      default: break;
    }
  };
  auto handle_inc = [&](bool){
    if (s_field == FT_SAVE) {
      int d_safe = (d < 1 ? 1 : (d > 31 ? 31 : d));
      datetimeSetManual(y, M, d_safe, h, mi, 0, datetimeGetTZMinutes());
      s_menuActive = false;
      g_dt_firstFrame = true; g_dt_blockUntil = 0; g_dt_primedPrev = false;
      return;
    }
    if (s_field == FT_CANCEL) {
      s_menuActive = false;
      g_dt_firstFrame = true; g_dt_blockUntil = 0; g_dt_primedPrev = false;
      return;
    }
    switch (s_field) {
      case FT_DAY:   inc_wrap(d , 1 , 31);  break;
      case FT_MONTH: inc_wrap(M , 1 , 12);  break;
      case FT_YEAR:  inc_wrap(y , 2000, 2099); break;
      case FT_HOUR:  inc_wrap(h , 0 , 23);  break;
      case FT_MIN:   inc_wrap(mi, 0 , 59);  break;
      default: break;
    }
  };

  if (altRise && s_field <= FT_MIN) handle_dec(true);
  if (okRise) { handle_inc(true); }

  if (s_field <= FT_MIN) {
    if (altDown) {
      if (altHoldStart == 0) { altHoldStart = now; altLastRpt = now; }
      else if (now - altHoldStart >= HOLD_REPEAT_DELAY_MS && now - altLastRpt >= REPEAT_MS) {
        handle_dec(false); altLastRpt = now;
      }
    } else altHoldStart = 0;

    if (okDown) {
      if (okHoldStart == 0) { okHoldStart = now; okLastRpt = now; }
      else if (now - okHoldStart >= HOLD_REPEAT_DELAY_MS && now - okLastRpt >= REPEAT_MS) {
        handle_inc(false); okLastRpt = now;
      }
    } else okHoldStart = 0;
  } else {
    altHoldStart = okHoldStart = 0;
  }
}

// ======================================================================
// Conversión epoch -> componentes locales con tz
// ======================================================================
static bool epochToLocalYMDHMS(uint32_t ts, int tz_min,
                               int &Y, int &M_, int &D,
                               int &h, int &m, int &s) {
  if (ts == 0) return false;
  int64_t e = (int64_t)ts + (int64_t)tz_min * 60; // local
  if (e < 0) return false;

  s = (int)(e % 60); if (s < 0) s += 60;
  int64_t e_min = e / 60;
  m = (int)(e_min % 60); if (m < 0) m += 60;
  int64_t e_hr = e_min / 60;
  h = (int)(e_hr % 24);  if (h < 0) h += 24;

  int64_t z = e / 86400;
  z += 719468;
  int era = (z >= 0 ? z : z - 146096) / 146097;
  int doe = (int)(z - era * 146097);
  int yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
  int Y_ = yoe + era * 400;
  int doy = doe - (365*yoe + yoe/4 - yoe/100);
  int mp  = (5*doy + 2)/153;
  int D_ = doy - (153*mp+2)/5 + 1;
  int Mv = mp + (mp < 10 ? 3 : -9);
  Y_ += (Mv <= 2);

  Y = Y_; M_ = Mv; D = D_;
  return true;
}

void datetimeFormatEpoch(uint32_t ts, char* out, size_t n) {
  if (!out || n == 0) return;
  if (ts == 0) { snprintf(out, n, "--"); return; }
  int Y,M_,D,h,m,s;
  if (!epochToLocalYMDHMS(ts, s_tb.tz_minutes, Y,M_,D,h,m,s)) {
    snprintf(out, n, "--"); return;
  }
  snprintf(out, n, "%04d-%02d-%02d %02d:%02d", Y, M_, D, h, m);
}

void datetimeFormatEpochShort(uint32_t ts, char* out, size_t n) {
  if (!out || n == 0) return;
  if (ts == 0) { snprintf(out, n, "--"); return; }
  int Y,M_,D,h,m,s;
  if (!epochToLocalYMDHMS(ts, s_tb.tz_minutes, Y,M_,D,h,m,s)) {
    snprintf(out, n, "--"); return;
  }
  snprintf(out, n, "%02d/%02d %02d:%02d", D, M_, h, m);
}

void datetimeFormatEpoch_HHMM(uint32_t ts, char* out, size_t n) {
  if (!out || n == 0) return;
  if (ts == 0) { snprintf(out, n, "--:--"); return; }
  int Y,M_,D,h,m,s;
  if (!epochToLocalYMDHMS(ts, s_tb.tz_minutes, Y,M_,D,h,m,s)) {
    snprintf(out, n, "--:--"); return;
  }
  snprintf(out, n, "%02d:%02d", h, m);
}

void datetimeFormatEpoch_DDMMYY(uint32_t ts, char* out, size_t n) {
  if (!out || n == 0) return;
  if (ts == 0) { snprintf(out, n, "--/--/--"); return; }
  int Y,M_,D,h,m,s;
  if (!epochToLocalYMDHMS(ts, s_tb.tz_minutes, Y,M_,D,h,m,s)) {
    snprintf(out, n, "--/--/--"); return;
  }
  snprintf(out, n, "%02d/%02d/%02d", D, M_, (Y % 100));
}
