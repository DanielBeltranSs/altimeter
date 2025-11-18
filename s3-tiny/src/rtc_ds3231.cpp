#include "rtc_ds3231.h"

static uint8_t to_bcd(uint8_t v){ return uint8_t((v/10)<<4 | (v%10)); }
static uint8_t from_bcd(uint8_t v){ return uint8_t((v>>4)*10 + (v&0x0F)); }

bool ds3231_present(){
  Wire.beginTransmission(DS3231_ADDR);
  return (Wire.endTransmission()==0);
}

bool ds3231_read_ymdhms(int &Y,int &M,int &D,int &h,int &m,int &s){
  // Colocar puntero en 0x00
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write((uint8_t)0x00);
  if (Wire.endTransmission(false)!=0) return false;

  if (Wire.requestFrom(DS3231_ADDR, (uint8_t)7) != 7) return false;
  uint8_t reg_sec  = Wire.read();
  uint8_t reg_min  = Wire.read();
  uint8_t reg_hour = Wire.read();
  (void)Wire.read();              // día de semana (0x03) - lo ignoramos
  uint8_t reg_date = Wire.read(); // día
  uint8_t reg_mon  = Wire.read(); // mes (bit7 = century)
  uint8_t reg_year = Wire.read(); // 00..99 (2000..2099)

  s = from_bcd(reg_sec & 0x7F);
  m = from_bcd(reg_min & 0x7F);

  // 24h (bit6=0) o 12h (bit6=1)
  if (reg_hour & 0x40){
    // 12h
    int hh = from_bcd(reg_hour & 0x1F);
    bool pm = (reg_hour & 0x20)!=0;
    h = (hh%12) + (pm?12:0);
  } else {
    h = from_bcd(reg_hour & 0x3F);
  }

  D = from_bcd(reg_date & 0x3F);
  M = from_bcd(reg_mon  & 0x1F);
  int century = (reg_mon & 0x80) ? 2100 : 2000;
  Y = century + from_bcd(reg_year);

  return (Y>=2000 && Y<=2199 && M>=1 && M<=12 && D>=1 && D<=31 && h<=23 && m<=59 && s<=59);
}

bool ds3231_write_ymdhms(int Y,int M,int D,int h,int m,int s){
  if (Y<2000 || Y>2199) return false;
  uint8_t monreg = to_bcd(M);
  if (Y>=2100) monreg |= 0x80;  // set century bit

  Wire.beginTransmission(DS3231_ADDR);
  Wire.write((uint8_t)0x00);    // desde segundos
  Wire.write(to_bcd(s));
  Wire.write(to_bcd(m));
  Wire.write((uint8_t)to_bcd(h) & 0x3F);  // 24h, bit6=0
  Wire.write((uint8_t)1);                 // day-of-week (no usado)
  Wire.write(to_bcd(D));
  Wire.write(monreg);
  Wire.write(to_bcd((uint8_t)(Y%100)));
  return (Wire.endTransmission()==0);
}
