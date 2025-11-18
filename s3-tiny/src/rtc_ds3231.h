#pragma once
#include <Arduino.h>
#include <Wire.h>

#ifndef DS3231_ADDR
#define DS3231_ADDR 0x68
#endif

bool ds3231_present();
bool ds3231_read_ymdhms(int &Y,int &M,int &D,int &h,int &m,int &s); // UTC si DS3231_STORES_UTC=1
bool ds3231_write_ymdhms(int Y,int M,int D,int h,int m,int s);      // escribe en 24h
