// ====================================================
// Juego: Snake (versión no bloqueante en entradas y tiempos)
// ====================================================

#include <Arduino.h>
#include <U8g2lib.h>
#include "config.h"
#include "ui_module.h"
#include "snake.h"          // usa Direction, Point y las #define

// Declarado en ui_module.cpp
extern U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2;
// Flag global para saber si el juego está activo
extern bool gameSnakeRunning;

static Point     snake[MAX_SNAKE_LENGTH];
static int       snakeLength;
static Direction currentDir;
static Point     fruit;

static unsigned long lastMoveTime;
static const unsigned long moveInterval = 250;

static bool gameOver = false;
static int  score    = 0;

// Anti-rebote y flancos
static bool s_prevAlt=false, s_prevOk=false, s_prevMenu=false;
static uint32_t s_lastAltEdge=0, s_lastOkEdge=0, s_lastMenuEdge=0;
static const uint16_t EDGE_DEBOUNCE_MS = 40;

// Cooldown para giros (sustituye delay(40))
static uint32_t s_nextRotateAtMs = 0;
static const uint16_t ROTATE_COOLDOWN_MS = 80;

// “Game Over” no bloqueante (sustituye delay(3000))
static bool     s_showingGameOver = false;
static uint32_t s_gameOverUntilMs = 0;
static const uint16_t GAMEOVER_MS = 3000;

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
  // ===== Estado inicial (se evalúa en la primera llamada) =====
  static bool initialized = false;
  if (!initialized) {
    // Activo-alto: “pulsado” = HIGH. Inicializamos prevs para no bloquear.
    s_prevOk   = (digitalRead(BUTTON_OLED)     == HIGH);
    s_prevMenu = (digitalRead(BUTTON_MENU)     == HIGH);
    s_prevAlt  = (digitalRead(BUTTON_ALTITUDE) == HIGH);

    snakeLength  = 3;
    snake[0]     = { GRID_WIDTH / 2, GRID_HEIGHT / 2 };       // Cabeza
    snake[1]     = { GRID_WIDTH / 2, GRID_HEIGHT / 2 + 1 };
    snake[2]     = { GRID_WIDTH / 2, GRID_HEIGHT / 2 + 2 };
    currentDir   = UP;
    score        = 0;
    placeFruit();
    lastMoveTime = millis();
    gameOver     = false;

    s_nextRotateAtMs  = 0;
    s_showingGameOver = false;
    s_gameOverUntilMs = 0;

    s_lastAltEdge = s_lastOkEdge = s_lastMenuEdge = 0;

    initialized = true;
  }

  // ===== Entradas NO bloqueantes con flancos y debounce =====
  const bool altDown  = (digitalRead(BUTTON_ALTITUDE) == HIGH);  // activo-alto
  const bool okDown   = (digitalRead(BUTTON_OLED)     == HIGH);  // activo-alto
  const bool menuDown = (digitalRead(BUTTON_MENU)     == HIGH);  // activo-alto

  const bool altRiseRaw  = (altDown  && !s_prevAlt);   // flanco de subida = presionado
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
    initialized = false;
    gameSnakeRunning = false;     // <<< AVISAR que el juego terminó
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
      initialized = false;
      gameSnakeRunning = false;   // <<< cerrar modo juego
      return;
    }
    return;
  }

  // ===== Giros (cooldown en vez de delay(40)) =====
  if (now >= s_nextRotateAtMs) {
    if (menuRise) {
      currentDir = (Direction)((currentDir + 1) % 4);      // horario
      s_nextRotateAtMs = now + ROTATE_COOLDOWN_MS;
    } else if (altRise) {
      currentDir = (Direction)((currentDir + 3) % 4);      // antihorario
      s_nextRotateAtMs = now + ROTATE_COOLDOWN_MS;
    }
  }

  // ===== Avance por intervalo =====
  if (now - lastMoveTime >= moveInterval) {
    lastMoveTime = now;

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

  // ===== Dibujo del juego =====
  if (!s_showingGameOver) {
    drawSnakeGame();
  }
}
