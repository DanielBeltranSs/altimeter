#ifndef UI_MODULE_H
#define UI_MODULE_H

#include <U8g2lib.h>

// Se declara el objeto de la pantalla y variables de UI
extern U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;
extern bool startupDone;
extern int menuOpcion;
extern bool menuActivo;
extern bool pantallaEncendida;

// Constantes para el menú
extern const int TOTAL_OPCIONES;
extern const int OPCIONES_POR_PAGINA;

// Funciones de la interfaz de usuario
void initUI();
void updateUI();
void dibujarMenu();
void mostrarCuentaRegresiva();
void processMenu();

#endif
