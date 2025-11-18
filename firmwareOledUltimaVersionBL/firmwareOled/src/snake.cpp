// ====================================================
// Juego: Snake
// ====================================================

#include <Arduino.h>
#include <U8g2lib.h>
#include "config.h"
#include "ui_module.h"
#include "snake.h"          // usa Direction, Point y las #define

// Declarado en ui_module.cpp
extern U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;

static Point     snake[MAX_SNAKE_LENGTH];
static int       snakeLength;
static Direction currentDir;
static Point     fruit;

static unsigned long lastMoveTime;
static const unsigned long moveInterval = 250;

static bool gameOver = false;
static int  score    = 0;

static inline bool isPointOnSnake(Point p) {
  for (int i = 0; i < snakeLength; i++) {
    if (snake[i].x == p.x && snake[i].y == p.y) return true;
  }
  return false;
}

static void placeFruit() {
  do {
    fruit.x = random(0, GRID_WIDTH);
    fruit.y = random(0, GRID_HEIGHT);
  } while (isPointOnSnake(fruit));
}

static void drawSnakeGame() {
  u8g2.clearBuffer();

  // Serpiente
  for (int i = 0; i < snakeLength; i++) {
    const int px = snake[i].x * CELL_SIZE;
    const int py = snake[i].y * CELL_SIZE;
    u8g2.drawBox(px, py, CELL_SIZE, CELL_SIZE);
  }

  // Fruta (marco)
  const int fx = fruit.x * CELL_SIZE;
  const int fy = fruit.y * CELL_SIZE;
  u8g2.drawFrame(fx, fy, CELL_SIZE, CELL_SIZE);

  // Puntuación
  u8g2.setFont(u8g2_font_5x7_tr);
  u8g2.setCursor(0, 7);
  u8g2.print("Score: ");
  u8g2.print(score);

  u8g2.sendBuffer();
}

void playSnakeGame() {
  // Limpiar pulsación previa de salida
  while (digitalRead(BUTTON_OLED) == LOW) { delay(10); }

  // Estado inicial
  snakeLength  = 3;
  snake[0]     = { GRID_WIDTH / 2, GRID_HEIGHT / 2 };       // Cabeza
  snake[1]     = { GRID_WIDTH / 2, GRID_HEIGHT / 2 + 1 };
  snake[2]     = { GRID_WIDTH / 2, GRID_HEIGHT / 2 + 2 };
  currentDir   = UP;     // <-- ahora sí usa el tipo de snake.h
  score        = 0;
  placeFruit();
  lastMoveTime = millis();
  gameOver     = false;

  // Bucle principal
  while (!gameOver) {
    // Salir con BUTTON_OLED
    if (digitalRead(BUTTON_OLED) == LOW) {
      while (digitalRead(BUTTON_OLED) == LOW) { delay(10); }
      return;
    }

    // Giro horario: BUTTON_MENU
    if (digitalRead(BUTTON_MENU) == LOW) {
      currentDir = (Direction)((currentDir + 1) % 4);
      while (digitalRead(BUTTON_MENU) == LOW) { delay(10); }
      delay(40);
    }

    // Giro antihorario: BUTTON_ALTITUDE
    if (digitalRead(BUTTON_ALTITUDE) == LOW) {
      currentDir = (Direction)((currentDir + 3) % 4); // -1 mod 4
      while (digitalRead(BUTTON_ALTITUDE) == LOW) { delay(10); }
      delay(40);
    }

    // Avance por intervalo
    if (millis() - lastMoveTime >= moveInterval) {
      lastMoveTime = millis();

      Point newHead = snake[0];
      switch (currentDir) {
        case UP:    newHead.y--; break;
        case DOWN:  newHead.y++; break;
        case LEFT:  newHead.x--; break;
        case RIGHT: newHead.x++; break;
      }

      // Colisiones
      if (newHead.x < 0 || newHead.x >= GRID_WIDTH ||
          newHead.y < 0 || newHead.y >= GRID_HEIGHT ||
          isPointOnSnake(newHead)) {
        gameOver = true;
      }

      if (gameOver) {
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_fub17_tr);
        {
          const char* msg = "Game Over";
          int x = (128 - u8g2.getStrWidth(msg)) / 2;
          if (x < 0) x = 0;
          u8g2.setCursor(x, 30);
          u8g2.print(msg);
        }
        u8g2.setFont(u8g2_font_5x7_tr);
        u8g2.setCursor(0, 60);
        u8g2.print("Score: ");
        u8g2.print(score);
        u8g2.sendBuffer();
        delay(3000);
        return;
      }

      // Desplazar cuerpo
      for (int i = snakeLength - 1; i > 0; i--) snake[i] = snake[i - 1];
      snake[0] = newHead;

      // Comer fruta
      if (newHead.x == fruit.x && newHead.y == fruit.y) {
        if (snakeLength < MAX_SNAKE_LENGTH) {
          snake[snakeLength] = snake[snakeLength - 1];
          snakeLength++;
        }
        score += 10;
        placeFruit();
      }
    }

    drawSnakeGame();
  }
}
