#define BLYNK_TEMPLATE_ID  "TMPL5dkPD9QQa"
#define BLYNK_TEMPLATE_NAME "XOProject"
#define BLYNK_AUTH_TOKEN    "W1Qx97Pd5BHN8W_xhCYBYJEnQCOCBF7G"
#define BLYNK_PRINT Serial
#define BUTTON_GREEN "#23C48E"
#define BUTTON_GREY  "#AAAAAA"

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <freertos/semphr.h>
#include <freertos/event_groups.h>

#include <queue>
std::queue<uint8_t> xMoves;
std::queue<uint8_t> oMoves;

BlynkTimer timer;

char ssid[] = "network";
char pass[] = "********";

LiquidCrystal_I2C lcd(0x27, 20, 4);

static const EventBits_t DISPLAY_UPDATE_STATUS = BIT3;
static const EventBits_t DISPLAY_UPDATE_SCORE  = BIT4;
static const EventBits_t DISPLAY_UPDATE_BOARD  = BIT5;
static const EventBits_t DISPLAY_MENU_SELECTION = BIT6;

String currentStatus = "";
String menuSelection = "";
bool refreshScore = false;
bool refreshBoard = false;

bool showMenuTemp = false;
unsigned long menuDisplayTime = 0;

// Handeled by GameCoordinatorTask
char board[9];
char currentPlayer;  // X or O
bool gameOver = false;
uint16_t scoreX = 0, scoreO = 0;     
const uint8_t WIN[8][3] = {
  {0,1,2},{3,4,5},{6,7,8},
  {0,3,6},{1,4,7},{2,5,8},
  {0,4,8},{2,4,6}
};

static EventGroupHandle_t egTurns;
static const EventBits_t X_TURN_BIT    = BIT0;
static const EventBits_t O_TURN_BIT    = BIT1;
static const EventBits_t GAME_OVER_BIT = BIT2;

static QueueHandle_t qX, qO;

typedef struct { char player; uint8_t idx; } MoveMsg;
static QueueHandle_t qCoord;

static SemaphoreHandle_t uiMutex;

void showStatus(const String &msg) {
  xSemaphoreTake(uiMutex, portMAX_DELAY);
  currentStatus = msg;
  xSemaphoreGive(uiMutex);
  xEventGroupSetBits(egTurns, DISPLAY_UPDATE_STATUS);
}

void updateScoreboard() {
  xSemaphoreTake(uiMutex, portMAX_DELAY);
  refreshScore = true;
  xSemaphoreGive(uiMutex);
  xEventGroupSetBits(egTurns, DISPLAY_UPDATE_SCORE);
}

void paintSquare(uint8_t i) {
  board[i] = board[i]; 
  xEventGroupSetBits(egTurns, DISPLAY_UPDATE_BOARD);
}

static bool threeInRow(char p) {
  for (auto &w : WIN)
    if (board[w[0]] == p && board[w[1]] == p && board[w[2]] == p) return true;
  return false;
}
static bool boardFull() {
  for (char c : board) if (c == ' ') return false;
  return true;
}

struct MoveRecord {
  char player;
  uint8_t idx;
};
MoveRecord moveHistory[9];  // 9 moves at max
uint8_t moveCount = 0;

void startNewGame() {
  while (!xMoves.empty()) xMoves.pop();
  while (!oMoves.empty()) oMoves.pop();

  xQueueReset(qX);
  xQueueReset(qO);

  moveCount = 0;
  Blynk.setProperty(V10, "isDisabled", true);  

  for (uint8_t i = 0; i < 9; i++) {
    board[i] = ' ';
    xSemaphoreTake(uiMutex, portMAX_DELAY);

    // Reset label (V11–V19)
    Blynk.virtualWrite(V11 + i, " ");

    // Reset button (V1–V9)
    Blynk.setProperty(V1 + i, "color", BUTTON_GREEN);
    Blynk.setProperty(V1 + i, "isDisabled", false);
    Blynk.virtualWrite(V1 + i, 0);  

    xSemaphoreGive(uiMutex);
  }

  currentPlayer = random(0, 2) ? 'X' : 'O';
  gameOver = false;

  xEventGroupClearBits(egTurns, X_TURN_BIT | O_TURN_BIT | GAME_OVER_BIT);
  xEventGroupSetBits(egTurns, currentPlayer == 'X' ? X_TURN_BIT : O_TURN_BIT);

  showStatus(String("Player ") + currentPlayer + " turn");
  updateScoreboard();
}

bool wasConnected = false;

void checkBlynkConnection() {
  bool isConnected = Blynk.connected();
  if (isConnected && !wasConnected) {
    Serial.println("App connected – resuming without score reset");
    startNewGame();  // Only start a new round, without reset the scores
  }
  wasConnected = isConnected;
}

// Player task
void PlayerTask(char symbol, QueueHandle_t myQ, EventBits_t MY_BIT) {
  uint8_t idx;
  EventBits_t bits;
  for (;;) {
    bits = xEventGroupWaitBits(
        egTurns,
        MY_BIT | GAME_OVER_BIT,
        pdFALSE,
        pdFALSE,
        portMAX_DELAY
    );

    if (bits & GAME_OVER_BIT) continue;

    if (xQueueReceive(myQ, &idx, portMAX_DELAY) == pdTRUE) {
      MoveMsg m = { symbol, idx };
      xQueueSend(qCoord, &m, portMAX_DELAY);
    }
  }
}
void PlayerXTask(void *p) { PlayerTask('X', qX, X_TURN_BIT); }
void PlayerOTask(void *p) { PlayerTask('O', qO, O_TURN_BIT); }

// Game coordination task
void GameCoordinatorTask(void *p) {
  MoveMsg m;
  startNewGame();
  for (;;) {
    if (xQueueReceive(qCoord, &m, portMAX_DELAY) == pdTRUE) {
      if (gameOver) continue;
      if (m.player != currentPlayer) continue;
      if (m.idx > 8 || board[m.idx] != ' ') continue;

      if (moveCount < 9) {
        moveHistory[moveCount++] = { m.player, m.idx };
      }

      if (m.player == 'X') xMoves.push(m.idx);
      else oMoves.push(m.idx);

      board[m.idx] = m.player;
      paintSquare(m.idx);

      bool win  = threeInRow(m.player);
      bool draw = !win && boardFull();
      if (win) {
        updateScoreboard();
        gameOver = true;
        xEventGroupSetBits(egTurns, GAME_OVER_BIT);
        showStatus(String("Player ") + m.player + " wins!");
        (m.player == 'X' ? scoreX : scoreO)++;
        vTaskDelay(pdMS_TO_TICKS(1000));
        startNewGame();  
        continue;
      } else if (draw) {
        showStatus("Draw!");
        gameOver = true;
        xEventGroupSetBits(egTurns, GAME_OVER_BIT);
        Blynk.setProperty(V10, "isDisabled", false);  // Enable the undo menu
      }

      currentPlayer = (currentPlayer == 'X') ? 'O' : 'X';
      xEventGroupClearBits(egTurns, X_TURN_BIT | O_TURN_BIT);
      xEventGroupSetBits(egTurns, currentPlayer == 'X' ? X_TURN_BIT : O_TURN_BIT);
      showStatus(String("Player ") + currentPlayer + " turn");
    }
  }
}

// Lcd task 
void DisplayTask(void *p) {
  for (;;) {
    EventBits_t bits = xEventGroupWaitBits(
      egTurns,
      DISPLAY_UPDATE_STATUS | DISPLAY_UPDATE_SCORE | DISPLAY_UPDATE_BOARD | DISPLAY_MENU_SELECTION,
      pdTRUE,
      pdFALSE,
      pdMS_TO_TICKS(100)
    );

    xSemaphoreTake(uiMutex, portMAX_DELAY);

    if (bits & DISPLAY_UPDATE_STATUS) {
      Serial.println("Status: " + currentStatus);
      lcd.setCursor(0, 0);
      lcd.print("                    ");
      lcd.setCursor(0, 0);
      lcd.print(currentStatus);
      Blynk.virtualWrite(V0, currentStatus);
    }

    if (bits & DISPLAY_UPDATE_SCORE && refreshScore) {
      Serial.println("Score Update -> X: " + String(scoreX) + "  O: " + String(scoreO));
      lcd.setCursor(0, 1);
      lcd.print("X:" + String(scoreX) + "  O:" + String(scoreO) + "             ");
      Blynk.virtualWrite(V30, scoreX);
      Blynk.virtualWrite(V31, scoreO);
      refreshScore = false;
    }

    if (bits & DISPLAY_UPDATE_BOARD) {
      Serial.println("Board Update:");
      for (uint8_t i = 0; i < 9; ++i) {
        Serial.printf(" [%c]", board[i]);
        Blynk.virtualWrite(V11 + i, String(board[i]));
        Blynk.setProperty(V1 + i, "color", board[i] == ' ' ? BUTTON_GREEN : BUTTON_GREY);
        Blynk.setProperty(V1 + i, "isDisabled", board[i] != ' ');
      }
      Serial.println();
    }

    if (bits & DISPLAY_MENU_SELECTION) {
      Serial.println("Menu Selection: " + menuSelection);
      lcd.setCursor(0, 2);
      lcd.print("                    ");
      lcd.setCursor(0, 2);
      lcd.print(menuSelection);
      showMenuTemp = true;
      menuDisplayTime = millis(); 
    }

    if (showMenuTemp && (millis() - menuDisplayTime >= 2000)) {  // = 2s
      lcd.setCursor(0, 2);
      lcd.print("                    ");
      showMenuTemp = false;
      Serial.println("Cleared rollback message");
    }

    xSemaphoreGive(uiMutex);
  }
}

BLYNK_WRITE(V10) {
  int index = param.asInt();  // Get the menu index (0, 1, 2)
  uint8_t xUndo = 0, oUndo = 0;
  Serial.print("Index: "); Serial.println(index);

  switch (index) {
  case 0:
    Serial.println("--- 2 moves ---");
    menuSelection = "Rollback: 2 moves";
    xUndo = oUndo = 1;
    break;
  case 1:
    Serial.println("--- 4 moves ---");
    menuSelection = "Rollback: 4 moves";
    xUndo = oUndo = 2;
    break;
  case 2:
    Serial.println("--- 6 moves ---");
    menuSelection = "Rollback: 6 moves";
    xUndo = oUndo = 3;
    break;
  default:
    return;
  }

  xSemaphoreTake(uiMutex, portMAX_DELAY);
  showMenuTemp = true;
  menuDisplayTime = millis();
  xSemaphoreGive(uiMutex);
  xEventGroupSetBits(egTurns, DISPLAY_MENU_SELECTION);

  Serial.print("xMoves available: "); Serial.println(xMoves.size());
  Serial.print("oMoves available: "); Serial.println(oMoves.size());

  if (xMoves.size() < xUndo || oMoves.size() < oUndo) {
    showStatus("Not enough moves to undo");
    return;
  }

  // Undo X moves
  uint8_t count = 0;
  Serial.print("X Undo: "); Serial.println(xUndo);
  while (!xMoves.empty() && count < xUndo) {
    uint8_t idx = xMoves.front(); xMoves.pop();
    Serial.print("Undoing X move at: "); Serial.println(idx);
    board[idx] = ' ';
    xSemaphoreTake(uiMutex, portMAX_DELAY);
    Blynk.virtualWrite(V11 + idx, " ");
    Blynk.setProperty(V1 + idx, "color", BUTTON_GREEN);
    Blynk.setProperty(V1 + idx, "isDisabled", false);
    Blynk.virtualWrite(V1 + idx, 0);
    xSemaphoreGive(uiMutex);
    count++;
  }

  // Undo O moves
  count = 0;
  Serial.print("O Undo: "); Serial.println(oUndo);
  while (!oMoves.empty() && count < oUndo) {
    uint8_t idx = oMoves.front(); oMoves.pop();
    Serial.print("Undoing O move at: "); Serial.println(idx);
    board[idx] = ' ';
    xSemaphoreTake(uiMutex, portMAX_DELAY);
    Blynk.virtualWrite(V11 + idx, " ");
    Blynk.setProperty(V1 + idx, "color", BUTTON_GREEN);
    Blynk.setProperty(V1 + idx, "isDisabled", false);
    Blynk.virtualWrite(V1 + idx, 0);
    xSemaphoreGive(uiMutex);
    count++;
  }

  // Resume the game
  gameOver = false;
  xEventGroupClearBits(egTurns, GAME_OVER_BIT);
  xEventGroupSetBits(egTurns, currentPlayer == 'X' ? X_TURN_BIT : O_TURN_BIT);
  showStatus(String("Player ") + currentPlayer + " turn");
  Blynk.setProperty(V10, "isDisabled", true);
}

// Blynk task
void BlynkTask(void *p) {
  for (;;) {
    Blynk.run();
    timer.run();  
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

inline void enqueue(uint8_t idx) {
  EventBits_t b = xEventGroupGetBits(egTurns);
  if (b & X_TURN_BIT)      xQueueSend(qX, &idx, 0);
  else if (b & O_TURN_BIT) xQueueSend(qO, &idx, 0);
}
#define BTN_HANDLER(N) \
  BLYNK_WRITE(V##N) {                                 \
    if (param.asInt() == 1) {                         \
      enqueue(N - 1);                                 \
      Blynk.virtualWrite(V##N, 0);                    \
    }                                                 \
  }
BTN_HANDLER(1) BTN_HANDLER(2) BTN_HANDLER(3) BTN_HANDLER(4) BTN_HANDLER(5)
BTN_HANDLER(6) BTN_HANDLER(7) BTN_HANDLER(8) BTN_HANDLER(9)

BLYNK_WRITE(V0) {
  String t = param.asString();
  xSemaphoreTake(uiMutex, portMAX_DELAY);
  lcd.setCursor(0, 0); lcd.print("                    ");
  lcd.setCursor(0, 0); lcd.print(t);
  xSemaphoreGive(uiMutex);
}

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);
  lcd.init(); lcd.backlight();
  Blynk.begin(BLYNK_AUTH_TOKEN, ssid, pass);

  uiMutex = xSemaphoreCreateBinary(); xSemaphoreGive(uiMutex);
  egTurns = xEventGroupCreate();
  qX      = xQueueCreate(5, sizeof(uint8_t));
  qO      = xQueueCreate(5, sizeof(uint8_t));
  qCoord  = xQueueCreate(5, sizeof(MoveMsg));

  xTaskCreatePinnedToCore(PlayerXTask,         "P_X",   4096, nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(PlayerOTask,         "P_O",   4096, nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(GameCoordinatorTask, "Coord", 4096, nullptr, 3, nullptr, 0);
  xTaskCreatePinnedToCore(BlynkTask, "Blynk", 4096, nullptr, 1, nullptr, 1);
  xTaskCreatePinnedToCore(DisplayTask, "Display", 4096, nullptr, 1, nullptr, 1);

  timer.setInterval(1000L, checkBlynkConnection); 
}

void loop() {
}
