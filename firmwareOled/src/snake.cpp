// ====================================================
// Juego: Snake
// ====================================================

#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include "config.h"  // Para definición de pines, por ejemplo: BUTTON_ALTITUDE, BUTTON_MENU, BUTTON_OLED
#include "ui_module.h" 
// Parámetros del juego
#define GRID_WIDTH  16  // Número de columnas (16 * 8 = 128 píxeles de ancho)
#define GRID_HEIGHT 8   // Número de filas (8 * 8 = 64 píxeles de alto)
#define CELL_SIZE   8   // Tamaño en píxeles de cada celda
#define MAX_SNAKE_LENGTH 128

enum Direction { UP, RIGHT, DOWN, LEFT };

struct Point {
  int x;
  int y;
};

Point snake[MAX_SNAKE_LENGTH];
int snakeLength;
Direction currentDir;
Point fruit;

unsigned long lastMoveTime;
const unsigned long moveInterval = 250; // Tiempo entre movimientos (en ms)

bool gameOver = false;
int score = 0;

// Se asume que el objeto "u8g2" ya está declarado globalmente (por ejemplo, en ui_module.cpp)

bool isPointOnSnake(Point p) {
  for (int i = 0; i < snakeLength; i++) {
    if (snake[i].x == p.x && snake[i].y == p.y) {
      return true;
    }
  }
  return false;
}

void placeFruit() {
  do {
    fruit.x = random(0, GRID_WIDTH);
    fruit.y = random(0, GRID_HEIGHT);
  } while (isPointOnSnake(fruit));
}

void drawSnakeGame() {
  u8g2.clearBuffer();
  // Dibuja la serpiente
  for (int i = 0; i < snakeLength; i++) {
    int px = snake[i].x * CELL_SIZE;
    int py = snake[i].y * CELL_SIZE;
    u8g2.drawBox(px, py, CELL_SIZE, CELL_SIZE);
  }
  // Dibuja la fruta como un recuadro
  int fx = fruit.x * CELL_SIZE;
  int fy = fruit.y * CELL_SIZE;
  u8g2.drawFrame(fx, fy, CELL_SIZE, CELL_SIZE);
  // Dibuja la puntuación en la parte superior
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.setCursor(0, 7);
  u8g2.print("Score: ");
  u8g2.print(score);
  u8g2.sendBuffer();
}

void playSnakeGame() {
  // Inicialización del juego: posición inicial de la serpiente en el centro.
  snakeLength = 3;
  snake[0] = {GRID_WIDTH / 2, GRID_HEIGHT / 2};      // Cabeza
  snake[1] = {GRID_WIDTH / 2, GRID_HEIGHT / 2 + 1};
  snake[2] = {GRID_WIDTH / 2, GRID_HEIGHT / 2 + 2};
  currentDir = UP; // Dirección inicial
  score = 0;
  placeFruit();
  lastMoveTime = millis();
  gameOver = false;

  // Bucle del juego: se ejecuta hasta que ocurre una colisión o se presiona el botón de salida.
  while (!gameOver) {
    // Permitir salir del juego presionando BUTTON_OLED
    if (digitalRead(BUTTON_OLED) == LOW) {
      // Esperar a que se libere el botón para evitar reingresar inmediatamente
      while(digitalRead(BUTTON_OLED) == LOW) { delay(10); }
      return; // Salir del juego y volver al menú.
    }
    
    // Lectura de botones para cambiar la dirección.
    // Usamos BUTTON_MENU para girar a la derecha (sentido horario)
    if (digitalRead(BUTTON_MENU) == LOW) {
      currentDir = (Direction)((currentDir + 1) % 4);
      delay(150); // Debounce y evitar múltiples cambios rápidos
    }
    // Usamos BUTTON_ALTITUDE para girar a la izquierda (sentido antihorario)
    if (digitalRead(BUTTON_ALTITUDE) == LOW) {
      currentDir = (Direction)((currentDir + 3) % 4);  // Sumar 3 equivale a restar 1
      delay(150);
    }
    
    // Mover la serpiente a intervalos establecidos
    if (millis() - lastMoveTime >= moveInterval) {
      lastMoveTime = millis();
      
      // Calcular la nueva posición de la cabeza
      Point newHead = snake[0];
      switch (currentDir) {
        case UP:    newHead.y--; break;
        case DOWN:  newHead.y++; break;
        case LEFT:  newHead.x--; break;
        case RIGHT: newHead.x++; break;
      }
      
      // Comprobar colisiones con paredes
      if (newHead.x < 0 || newHead.x >= GRID_WIDTH ||
          newHead.y < 0 || newHead.y >= GRID_HEIGHT) {
        gameOver = true;
      }
      // Comprobar colisiones con el cuerpo de la serpiente
      if (isPointOnSnake(newHead)) {
        gameOver = true;
      }
      
      if (gameOver) {
        // Mostrar pantalla de fin del juego
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_fub17_tr);
        int x = (128 - u8g2.getStrWidth("Game Over")) / 2;
        u8g2.setCursor(x, 30);
        u8g2.print("Game Over");
        u8g2.setFont(u8g2_font_5x7_tr);
        u8g2.setCursor(0, 60);
        u8g2.print("Score: ");
        u8g2.print(score);
        u8g2.sendBuffer();
        delay(3000);
        return;
      }
      
      // Desplazar la serpiente: el último segmento se actualiza desde el anterior.
      for (int i = snakeLength - 1; i > 0; i--) {
        snake[i] = snake[i - 1];
      }
      snake[0] = newHead;
      
      // Si la serpiente come la fruta, se incrementa el tamaño y se actualiza la puntuación
      if (newHead.x == fruit.x && newHead.y == fruit.y) {
        if (snakeLength < MAX_SNAKE_LENGTH) {
          snake[snakeLength] = snake[snakeLength - 1];  // Duplicar el último segmento
          snakeLength++;
        }
        score += 10;
        placeFruit();
      }
    }
    drawSnakeGame();
  }
}
