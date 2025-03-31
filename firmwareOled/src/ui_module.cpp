#include "ui_module.h"
#include "config.h"
#include "ble_module.h"  // Para toggleBLE(), si se usa en alguna opción
#include <Arduino.h>
#include <U8g2lib.h>

// Se instancia el objeto de la pantalla
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
bool startupDone = false;

// Constantes para el menú
const int TOTAL_OPCIONES = 7;
const int OPCIONES_POR_PAGINA = 3;

// Declaraciones de variables externas de configuración
extern bool unidadMetros;
extern int brilloPantalla;
extern int altFormat;
extern bool bleActivo;
extern unsigned long ahorroTimeoutMs;
extern int ahorroTimeoutOption;
extern const unsigned long TIMEOUT_OPTIONS[];

// Variables globales para el menú (se pueden definir también en main.cpp)
int menuOpcion = 0;
bool menuActivo = false;

// Declaración de variables usadas en UI (por ejemplo, porcentaje de batería)

// Se asume que cachedBatteryPercentage se define en sensor_module.cpp y es accesible
extern int cachedBatteryPercentage;

// Función para inicializar la UI
void initUI() {
  u8g2.begin();
  u8g2.setPowerSave(false);
  u8g2.setContrast(brilloPantalla);
}

// Función para mostrar la cuenta regresiva de startup
void mostrarCuentaRegresiva() {
  static unsigned long startupStartTime = 0;
  if (startupStartTime == 0) startupStartTime = millis();
  unsigned long elapsed = millis() - startupStartTime;
  int secondsLeft = 5 - (elapsed / 1000);
  if (secondsLeft < 0) secondsLeft = 0;
  
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_fub30_tr);
  String cuenta = String(secondsLeft);
  int xPos = (128 - u8g2.getStrWidth(cuenta.c_str())) / 2;
  u8g2.setCursor(xPos, 40);
  u8g2.print(cuenta);
  
  u8g2.setFont(u8g2_font_ncenB08_tr);
  int w = u8g2.getStrWidth("Calibrando...");
  int x = (128 - w) / 2;
  u8g2.setCursor(x, 60);
  u8g2.print("Calibrando...");
  u8g2.sendBuffer();
  
  if (elapsed >= 5000) startupDone = true;
  delay(100);
}

// Función para dibujar el menú en pantalla
void dibujarMenu() {
  int paginaActual = menuOpcion / OPCIONES_POR_PAGINA;
  int totalPaginas = (TOTAL_OPCIONES + OPCIONES_POR_PAGINA - 1) / OPCIONES_POR_PAGINA;
  int inicio = paginaActual * OPCIONES_POR_PAGINA;
  int fin = inicio + OPCIONES_POR_PAGINA;
  if (fin > TOTAL_OPCIONES) fin = TOTAL_OPCIONES;
  
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.setCursor(0, 12);
  u8g2.print("MENU:");
  
  for (int i = inicio; i < fin; i++) {
    int y = 24 + (i - inicio) * 12;
    u8g2.setCursor(0, y);
    if (i == menuOpcion) {
      u8g2.print("> ");
    } else {
      u8g2.print("  ");
    }
    switch(i) {
      case 0:
        u8g2.print("Unidad: ");
        u8g2.print(unidadMetros ? "metros" : "pies");
        break;
      case 1:
        u8g2.print("Brillo: ");
        u8g2.print(brilloPantalla);
        break;
      case 2:
        u8g2.print("Altura: ");
        switch (altFormat) {
          case 0: u8g2.print("normal"); break;
          case 1: u8g2.print("1 decimal"); break;
          case 2: u8g2.print("2 decimales"); break;
          case 3: u8g2.print("3 decimales"); break;
        }
        break;
      case 3:
        u8g2.print("Bateria");
        break;
      case 4:
        u8g2.print("BL: ");
        u8g2.print(bleActivo ? "ON" : "OFF");
        break;
      case 5:
        u8g2.print("Invertir: ");
        u8g2.print(inversionActiva ? "ON" : "OFF");
        break;
      case 6:
        u8g2.print("Ahorro: ");
        if (ahorroTimeoutMs == 0)
          u8g2.print("OFF");
        else
          u8g2.print(String(ahorroTimeoutMs / 60000) + " min");
        break;
    }
  }
  
  u8g2.setCursor(100, 63);
  u8g2.print(String(paginaActual + 1) + "/" + String(totalPaginas));
  if (paginaActual > 0) {
    u8g2.setCursor(0, 63);
    u8g2.print("<");
  }
  if (paginaActual < totalPaginas - 1) {
    u8g2.setCursor(120, 63);
    u8g2.print(">");
  }
  u8g2.sendBuffer();
}

// Función que ejecuta la acción correspondiente a la opción del menú
static void ejecutarOpcionMenu(int opcion) {
  switch(opcion) {
    case 0: // Cambiar unidad
      unidadMetros = !unidadMetros;
      Serial.print("Unidad cambiada a: ");
      Serial.println(unidadMetros ? "metros" : "pies");
      break;
    case 1: // Cambiar brillo
      brilloPantalla += 50;
      if (brilloPantalla > 255) brilloPantalla = 50;
      u8g2.setContrast(brilloPantalla);
      Serial.print("Brillo cambiado a: ");
      Serial.println(brilloPantalla);
      break;
    case 2: // Cambiar formato de altitud
      altFormat = (altFormat + 1) % 4;
      Serial.print("Formato de altitud cambiado a: ");
      switch (altFormat) {
        case 0: Serial.println("normal"); break;
        case 1: Serial.println("1 decimal"); break;
        case 2: Serial.println("2 decimales"); break;
        case 3: Serial.println("3 decimales"); break;
      }
      break;
    case 3: // Acción para batería (puede quedar sin acción)
      Serial.println("Opción Batería seleccionada.");
      break;
    case 4: // Alternar BLE
      toggleBLE();
      Serial.print("BLE ahora está: ");
      Serial.println(bleActivo ? "ON" : "OFF");
      break;
    case 5: // Cambiar modo de inversión
      inversionActiva = !inversionActiva;
      if (inversionActiva) {
        u8g2.sendF("c", 0xA7);
        Serial.println("Display invertido.");
      } else {
        u8g2.sendF("c", 0xA6);
        Serial.println("Display en modo normal.");
      }
      break;
    case 6: // Cambiar modo ahorro
      ahorroTimeoutOption = (ahorroTimeoutOption + 1) % 4;
      ahorroTimeoutMs = TIMEOUT_OPTIONS[ahorroTimeoutOption];
      Serial.print("Modo ahorro configurado a: ");
      if (ahorroTimeoutMs == 0)
        Serial.println("OFF");
      else
        Serial.println(String(ahorroTimeoutMs / 60000) + " min");
      break;
  }
  saveConfig();
}

// Función para procesar la navegación del menú
void processMenu() {
  // Si se presiona el botón ALTITUDE, cambiar la opción del menú
  if (digitalRead(BUTTON_ALTITUDE) == LOW) {
    delay(50); // Debounce
    menuOpcion = (menuOpcion + 1) % TOTAL_OPCIONES;
    while (digitalRead(BUTTON_ALTITUDE) == LOW); // Esperar a soltar
    Serial.print("Opción del menú cambiada a: ");
    Serial.println(menuOpcion);
  }
  
  // Si se presiona el botón MENU, se cierra el menú
  if (digitalRead(BUTTON_MENU) == LOW) {
    delay(50);
    menuActivo = false;
    while (digitalRead(BUTTON_MENU) == LOW);
    Serial.println("Menú cerrado.");
  }
  
  // Si se presiona el botón OLED, confirmar la opción seleccionada
  if (digitalRead(BUTTON_OLED) == LOW) {
    delay(50);
    ejecutarOpcionMenu(menuOpcion);
    while (digitalRead(BUTTON_OLED) == LOW);
    menuActivo = false;
  }
}

// Función de actualización de la UI (se llama en cada loop)
void updateUI() {
  if (!startupDone) {
    mostrarCuentaRegresiva();
    return;
  }
  if (!menuActivo) {
    // Pantalla principal
    u8g2.clearBuffer();
    
    // Mostrar unidad (M o FT)
    u8g2.setFont(u8g2_font_ncenB08_tr);
    u8g2.setCursor(2, 12);
    u8g2.print(unidadMetros ? "M" : "FT");
    
    // Mostrar estado BLE centrado
    u8g2.setFont(u8g2_font_ncenB08_tr);
    String bleStr = "BL: " + String(bleActivo ? "ON" : "OFF");
    int xPosBle = (128 - u8g2.getStrWidth(bleStr.c_str())) / 2;
    u8g2.setCursor(xPosBle, 12);
    u8g2.print(bleStr);
    
    // Mostrar porcentaje de batería a la derecha
    u8g2.setFont(u8g2_font_ncenB08_tr);
    String batStr = String(cachedBatteryPercentage) + "%";
    int batWidth = u8g2.getStrWidth(batStr.c_str());
    u8g2.setCursor(128 - batWidth - 2, 12);
    u8g2.print(batStr);
    
    // Mostrar altitud (este ejemplo usa "123" como altitud; reemplaza por la variable real)
    u8g2.setFont(u8g2_font_fub30_tr);
    String altStr = "123";
    int xPosAlt = (128 - u8g2.getStrWidth(altStr.c_str())) / 2;
    if (xPosAlt < 0) xPosAlt = 0;
    u8g2.setCursor(xPosAlt, 50);
    u8g2.print(altStr);
    
    // Dibujar bordes (opcional)
    u8g2.drawHLine(0, 15, 128);
    u8g2.drawHLine(0, 52, 128);
    u8g2.drawHLine(0, 0, 128);
    u8g2.drawHLine(0, 63, 128);
    u8g2.drawVLine(0, 0, 64);
    u8g2.drawVLine(127, 0, 64);
    
    // Mostrar usuario en la parte inferior, centrado
    u8g2.setFont(u8g2_font_ncenB08_tr);
    String user = usuarioActual;
    int xPosUser = (128 - u8g2.getStrWidth(user.c_str())) / 2;
    if (xPosUser < 0) xPosUser = 0;
    u8g2.setCursor(xPosUser, 62);
    u8g2.print(user);
    
    u8g2.sendBuffer();
  }
  else {
    // Si el menú está activo, dibujar el menú
    dibujarMenu();
  }
}
