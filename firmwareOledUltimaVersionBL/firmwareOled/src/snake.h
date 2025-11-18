#ifndef SNAKE_H
#define SNAKE_H

#include <Arduino.h>
#include <U8g2lib.h>

// Definiciones de la rejilla del juego
#define GRID_WIDTH        16      // 16 * 8 = 128 px
#define GRID_HEIGHT       8       //  8 * 8 =  64 px
#define CELL_SIZE         8       // Tamaño en píxeles de cada celda
#define MAX_SNAKE_LENGTH  128     // Longitud máxima de la serpiente

extern bool gameSnakeRunning;

// Enumeración para las direcciones de movimiento
enum Direction {
  UP,
  RIGHT,
  DOWN,
  LEFT
};

// Estructura para representar un punto en la rejilla
struct Point {
  int x;
  int y;
};

// Inicia el juego Snake (bloquea hasta salir del juego)
void playSnakeGame();

#endif // SNAKE_H
