// ====================================================
// Juego: Snake (versión no bloqueante en entradas y tiempos)
// ====================================================

#include <Arduino.h>
#include <U8g2lib.h>
#include "config.h"
#include "ui_module.h"
#include "snake.h"          // Direction, Point, GRID_*, CELL_SIZE, MAX_SNAKE_LENGTH

// Flag global para saber si el juego está activo
extern bool gameSnakeRunning;

// u8g2 viene del módulo de UI
extern U8G2_ST7567_JLX12864_F_4W_SW_SPI u8g2;

static Point     snake[MAX_SNAKE_LENGTH];
static int       snakeLength;
static Direction currentDir;
static Point     fruit;

static unsigned long lastMoveTime;
static const unsigned long moveInterval = 250;   // ms por paso

static bool gameOver = false;
static int  score    = 0;

// Anti-rebote y flancos
static bool s_prevAlt=false, s_prevOk=false, s_prevMenu=false;
static uint32_t s_lastAltEdge=0, s_lastOkEdge=0, s_lastMenuEdge=0;
static const uint16_t EDGE_DEBOUNCE_MS = 40;

// Cooldown suave para giros (además del latch por paso)
static uint32_t s_nextRotateAtMs = 0;
static const uint16_t ROTATE_COOLDOWN_MS = 70;

// “Game Over” no bloqueante
static bool     s_showingGameOver = false;
static uint32_t s_gameOverUntilMs = 0;
static const uint16_t GAMEOVER_MS = 1200;

// Latch: SOLO 1 rotación permitida entre pasos
static bool s_rotatedThisStep = false;

// Helpers de dirección (asumen enum en orden UP=0, RIGHT=1, DOWN=2, LEFT=3)
static inline Direction turnCW(Direction d)  { return (Direction)((((int)d) + 1) & 3); }
static inline Direction turnCCW(Direction d) { return (Direction)((((int)d) + 3) & 3); }
static inline bool isOpposite(Direction a, Direction b) {
  // pares opuestos: (UP,DOWN) -> (0,2) y (RIGHT,LEFT) -> (1,3)
  return (((int)a ^ (int)b) == 2);
}

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
  // ===== Estado inicial (una sola vez) =====
  static bool initialized = false;
  if (!initialized) {
    // Inicializar flancos con estado actual (activo-alto)
    s_prevOk   = (digitalRead(BUTTON_OLED)     == HIGH);
    s_prevMenu = (digitalRead(BUTTON_MENU)     == HIGH);
    s_prevAlt  = (digitalRead(BUTTON_ALTITUDE) == HIGH);

    // Serpiente centrada (dir UP por defecto)
    snakeLength  = 3;
    snake[0]     = { GRID_WIDTH / 2, GRID_HEIGHT / 2 };       // Cabeza
    snake[1]     = { GRID_WIDTH / 2, GRID_HEIGHT / 2 + 1 };
    snake[2]     = { GRID_WIDTH / 2, GRID_HEIGHT / 2 + 2 };
    currentDir   = UP;
    score        = 0;

    // Fruta
    #if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
      randomSeed(esp_random());
    #endif
    placeFruit();

    lastMoveTime = millis();
    gameOver     = false;

    s_nextRotateAtMs  = 0;
    s_showingGameOver = false;
    s_gameOverUntilMs = 0;
    s_rotatedThisStep = false;

    s_lastAltEdge = s_lastOkEdge = s_lastMenuEdge = 0;

    initialized = true;
  }

  // ===== Entradas NO bloqueantes con flancos y debounce =====
  const bool altDown  = (digitalRead(BUTTON_ALTITUDE) == HIGH);  // activo-alto
  const bool okDown   = (digitalRead(BUTTON_OLED)     == HIGH);  // activo-alto
  const bool menuDown = (digitalRead(BUTTON_MENU)     == HIGH);  // activo-alto

  const bool altRiseRaw  = (altDown  && !s_prevAlt);
  const bool okRiseRaw   = (okDown   && !s_prevOk);
  const bool menuRiseRaw = (menuDown && !s_prevMenu);

  const uint32_t now = millis();
  const bool altRise  = altRiseRaw  && (now - s_lastAltEdge  > EDGE_DEBOUNCE_MS);
  const bool okRise   = okRiseRaw   && (now - s_lastOkEdge   > EDGE_DEBOUNCE_MS);
  const bool menuRise = menuRiseRaw && (now - s_lastMenuEdge > EDGE_DEBOUNCE_MS);

  if (altRise)  s_lastAltEdge  = now;
  if (okRise)   s_lastOkEdge   = now;
  if (menuRise) s_lastMenuEdge = now;

  s_prevAlt  = altDown;
  s_prevOk   = okDown;
  s_prevMenu = menuDown;

  // ===== Salida con tap en OK (no bloqueante) =====
  if (okRise && !s_showingGameOver) {
  u8g2.clearBuffer();           // blanquea frame del juego
  u8g2.sendBuffer();
  uiRequestRefresh();           // <<< fuerza repintado de la UI
  initialized = false;
  gameSnakeRunning = false;
  return;
  }

  // ===== Lógica de “Game Over” no bloqueante =====
  if (s_showingGameOver) {
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

    if (now >= s_gameOverUntilMs || okRise) {
      u8g2.clearBuffer();
      u8g2.sendBuffer();
      uiRequestRefresh();           // <<< idem
      initialized = false;
      gameSnakeRunning = false;
      return;
    }
    return;
  }

  // ===== Giros (una rotación por paso + sin U-turn) =====
  if (!s_rotatedThisStep && now >= s_nextRotateAtMs) {
    Direction next = currentDir;
    if (menuRise)      next = turnCW(currentDir);   // menú = horario
    else if (altRise)  next = turnCCW(currentDir);  // alt = antihorario

    if (next != currentDir && !isOpposite(next, currentDir)) {
      currentDir = next;
      s_rotatedThisStep = true;                     // latch hasta el próximo paso
      s_nextRotateAtMs  = now + ROTATE_COOLDOWN_MS;
    }
  }

  // ===== Avance por intervalo =====
  if (now - lastMoveTime >= moveInterval) {
    lastMoveTime = now;
    s_rotatedThisStep = false;                      // liberar latch para el nuevo paso

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
      s_showingGameOver = true;
      s_gameOverUntilMs = now + GAMEOVER_MS;
    } else {
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
  }

  // ===== Dibujo =====
  if (!s_showingGameOver) {
    drawSnakeGame();
  }
}
