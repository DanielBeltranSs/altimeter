#ifndef SNAKE_H
#define SNAKE_H

#include <Arduino.h>
#include <U8g2lib.h>

// Definiciones de la rejilla del juego
#define GRID_WIDTH       16      // Número de columnas (16 * 8 = 128 píxeles)
#define GRID_HEIGHT      8       // Número de filas (8 * 8 = 64 píxeles)
#define CELL_SIZE        8       // Tamaño en píxeles de cada celda
#define MAX_SNAKE_LENGTH 128     // Longitud máxima de la serpiente

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

// Declaración de la función principal del juego
// Al llamarla se inicia el juego Snake
void playSnakeGame();

#endif // SNAKE_H
