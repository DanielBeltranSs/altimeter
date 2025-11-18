#ifndef UI_MODULE_H
#define UI_MODULE_H

#include <U8g2lib.h>

// ---- Objetos/estado UI expuestos ----
extern U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;
extern bool startupDone;
extern int  menuOpcion;
extern bool menuActivo;
extern bool pantallaEncendida;   // Se usa fuera para mostrar/ocultar

extern long lastMenuInteraction; // Para timeouts y ahorro

// ---- API UI ----
void initUI();                  // Inicializa U8g2, contraste, inversión, etc.
void updateUI();                // Dibuja HUD/menús de forma no bloqueante
void dibujarMenu();             // Render del menú principal
void mostrarCuentaRegresiva();  // Splash de arranque (no bloqueante)
void processMenu();             // Lógica de navegación del menú (no bloqueante)

// (Opcional) Si algún módulo externo quisiera invocar la pantalla de edición:
// void dibujarOffsetEdit();

// ---- (Nuevo, para mostrar estado de carga en UI sin incluir headers) ----
bool     isUsbPresent();   // true si hay VBUS presente (con histéresis)
uint16_t readVBUSmV();     // VBUS estimado en mV (del divisor 330k/510k)

#endif // UI_MODULE_H
