#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>

// --- TFT DISPLAY PINS ---
#define TFT_CS         27
#define TFT_RST        26
#define TFT_DC         16

// --- ORIGINAL BUTTON PINS ---
#define BTN_UP        12
#define BTN_DOWN      14
#define BTN_SELECT    13

// --- CUSTOM COLOR DEFINITIONS ---
#define ST7735_DARKGREEN 0x03E0
#define ST7735_DARKGRAY  0x39E7
#define ST7735_NAVY      0x000F

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);

// --- OS CONCEPT 1: FREE RTOS INTER-THREAD QUEUE ---
enum ButtonEvent { EVENT_UP, EVENT_DOWN, EVENT_SELECT };
QueueHandle_t buttonQueue;

// --- OS CONCEPT 2: SYSTEM STATE MACHINE ---
enum SystemState { 
  STATE_WELCOME, 
  STATE_MAIN_MENU, 
  STATE_PC_STATS, 
  STATE_START_FETCHING, 
  STATE_START_LIST, 
  STATE_END_FETCHING, 
  STATE_END_LIST 
};
SystemState currentState = STATE_WELCOME;

// Main Menu Hardcoded Items
const char* mainMenu[] = {
  "1. PC Stats",
  "2. Start Processes",
  "3. End Processes",
  "4. Lock PC"
};
const int mainMenuItemCount = 4;

// Dynamic Lists Memory Allocation
String dynamicStartApps[16];
int dynamicStartCount = 0;

String dynamicEndApps[16];
int dynamicEndCount = 0;

int currentSelection = 0;

// --- FREERTOS THREAD: BUTTON SCANNING TASK (CORE 0) ---
void TaskButtonScan(void *pvParameters) {
  unsigned long lastPress = 0;
  const unsigned long debounceMs = 180;

  for (;;) {
    unsigned long now = millis();
    if (now - lastPress >= debounceMs) {
      if (digitalRead(BTN_UP) == LOW) {
        ButtonEvent evt = EVENT_UP;
        xQueueSend(buttonQueue, &evt, 0); // Non-blocking queue push
        lastPress = now;
      } 
      else if (digitalRead(BTN_DOWN) == LOW) {
        ButtonEvent evt = EVENT_DOWN;
        xQueueSend(buttonQueue, &evt, 0);
        lastPress = now;
      } 
      else if (digitalRead(BTN_SELECT) == LOW) {
        ButtonEvent evt = EVENT_SELECT;
        xQueueSend(buttonQueue, &evt, 0);
        lastPress = now;
      }
    }
    // Context switch yield to prevent watchdog resets
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// Forward UI Declarations
void renderWelcomeScreen();
void renderMainMenu();
void renderFetchingScreen(const char* title);
void renderDynamicList(const char* title, String items[], int count);
void renderTelemetryInit();
void updateTelemetryUI(int cpu, int ram);
void parseDynamicMenu(String jsonStr, String itemsTarget[], int &countTarget, const char* jsonKey);

void setup() {
  Serial.begin(115200);

  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);

  tft.initR(INITR_BLACKTAB);
  tft.setRotation(1); 
  tft.fillScreen(ST7735_BLACK);

  // OS CONCEPT: Create FIFO Queue (up to 10 events)
  buttonQueue = xQueueCreate(10, sizeof(ButtonEvent));

  // OS CONCEPT: Pin Button Thread onto Core 0 (System Core)
  xTaskCreatePinnedToCore(
    TaskButtonScan,   
    "ButtonTask", 
    2048,             
    NULL,             
    1,                
    NULL,             
    0                 
  );

  renderWelcomeScreen();
}

// MAIN THREAD (RUNS ON CORE 1)
void loop() {
  ButtonEvent evt;

  // OS CONCEPT: Asynchronously receive button events from Core 0
  if (xQueueReceive(buttonQueue, &evt, 0) == pdTRUE) {
    
    // 1. WELCOME SCREEN
    if (currentState == STATE_WELCOME) {
      if (evt == EVENT_SELECT) {
        currentState = STATE_MAIN_MENU;
        currentSelection = 0;
        renderMainMenu();
      }
    }

    // 2. MAIN MENU NAVIGATION
    else if (currentState == STATE_MAIN_MENU) {
      if (evt == EVENT_UP) {
        currentSelection = (currentSelection - 1 + mainMenuItemCount) % mainMenuItemCount;
        renderMainMenu();
      }
      else if (evt == EVENT_DOWN) {
        currentSelection = (currentSelection + 1) % mainMenuItemCount;
        renderMainMenu();
      }
      else if (evt == EVENT_SELECT) {
        if (currentSelection == 0) { // PC Stats
          currentState = STATE_PC_STATS;
          Serial.println("CMD:START_TELEMETRY");
          renderTelemetryInit();
        } 
        else if (currentSelection == 1) { // Start Processes
          currentState = STATE_START_FETCHING;
          renderFetchingScreen("START PROCESSES");
          Serial.println("CMD:GET_START_MENU");
        } 
        else if (currentSelection == 2) { // End Processes
          currentState = STATE_END_FETCHING;
          renderFetchingScreen("END PROCESSES");
          Serial.println("CMD:GET_END_MENU");
        }
        else if(currentSelection ==3){
          Serial.println("CMD:LOCK_PC");
        }
        
      }
    }

    // 3. PC STATS SCREEN
    else if (currentState == STATE_PC_STATS) {
      if (evt == EVENT_SELECT) {
        Serial.println("CMD:STOP_TELEMETRY");
        currentState = STATE_MAIN_MENU;
        currentSelection = 0;
        renderMainMenu();
      }
    }

    // 4. START PROCESSES LIST
    else if (currentState == STATE_START_LIST) {
      if (evt == EVENT_UP) {
        currentSelection = (currentSelection - 1 + dynamicStartCount) % dynamicStartCount;
        renderDynamicList("START PROCESSES", dynamicStartApps, dynamicStartCount);
      }
      else if (evt == EVENT_DOWN) {
        currentSelection = (currentSelection + 1) % dynamicStartCount;
        renderDynamicList("START PROCESSES", dynamicStartApps, dynamicStartCount);
      }
      else if (evt == EVENT_SELECT) {
        if (dynamicStartApps[currentSelection] == "< Back") {
          currentState = STATE_MAIN_MENU;
          currentSelection = 1;
          renderMainMenu();
        } else {
          Serial.print("CMD:LAUNCH:");
          Serial.println(dynamicStartApps[currentSelection]);
        }
      }
    }

    // 5. END PROCESSES LIST
    else if (currentState == STATE_END_LIST) {
      if (evt == EVENT_UP) {
        currentSelection = (currentSelection - 1 + dynamicEndCount) % dynamicEndCount;
        renderDynamicList("END PROCESSES", dynamicEndApps, dynamicEndCount);
      }
      else if (evt == EVENT_DOWN) {
        currentSelection = (currentSelection + 1) % dynamicEndCount;
        renderDynamicList("END PROCESSES", dynamicEndApps, dynamicEndCount);
      }
      else if (evt == EVENT_SELECT) {
        if (dynamicEndApps[currentSelection] == "< Back") {
          currentState = STATE_MAIN_MENU;
          currentSelection = 2;
          renderMainMenu();
        } else {
          Serial.print("CMD:KILL:");
          Serial.println(dynamicEndApps[currentSelection]);
          // Refresh active processes list after termination
          currentState = STATE_END_FETCHING;
          renderFetchingScreen("END PROCESSES");
          Serial.println("CMD:GET_END_MENU");
        }
      }
    }
  }

  // LISTEN FOR SERIAL DATA ON CORE 1
  if (Serial.available() > 0) {
    String line = Serial.readStringUntil('\n');
    line.trim();

    // Telemetry Update
    if (currentState == STATE_PC_STATS) {
      int cpuIdx = line.indexOf("\"cpu\":");
      int ramIdx = line.indexOf("\"ram\":");
      if (cpuIdx != -1 && ramIdx != -1) {
        int cpuVal = line.substring(cpuIdx + 6).toInt();
        int ramVal = line.substring(ramIdx + 6).toInt();
        updateTelemetryUI(cpuVal, ramVal);
      }
    }
    // Start Menu Response
    else if (currentState == STATE_START_FETCHING && line.indexOf("start_apps") != -1) {
      parseDynamicMenu(line, dynamicStartApps, dynamicStartCount, "start_apps");
      currentState = STATE_START_LIST;
      currentSelection = 0;
      renderDynamicList("START PROCESSES", dynamicStartApps, dynamicStartCount);
    }
    // End Menu Response
    else if (currentState == STATE_END_FETCHING && line.indexOf("end_apps") != -1) {
      parseDynamicMenu(line, dynamicEndApps, dynamicEndCount, "end_apps");
      currentState = STATE_END_LIST;
      currentSelection = 0;
      renderDynamicList("END PROCESSES", dynamicEndApps, dynamicEndCount);
    }
  }

  vTaskDelay(pdMS_TO_TICKS(5));
}

// --- DYNAMIC JSON PARSER (Zero Library Dependency) ---
void parseDynamicMenu(String jsonStr, String itemsTarget[], int &countTarget, const char* jsonKey) {
  countTarget = 0;
  int keyIdx = jsonStr.indexOf(jsonKey);
  if (keyIdx == -1) return;

  int startArr = jsonStr.indexOf('[', keyIdx);
  int endArr = jsonStr.indexOf(']', startArr);
  if (startArr == -1 || endArr == -1) return;

  String arrContent = jsonStr.substring(startArr + 1, endArr);
  int curPos = 0;

  while (curPos < arrContent.length() && countTarget < 16) {
    int q1 = arrContent.indexOf('"', curPos);
    if (q1 == -1) break;
    int q2 = arrContent.indexOf('"', q1 + 1);
    if (q2 == -1) break;

    itemsTarget[countTarget] = arrContent.substring(q1 + 1, q2);
    countTarget++;
    curPos = q2 + 1;
  }
}

// --- DISPLAY UI FUNCTIONS ---
void renderWelcomeScreen() {
  tft.fillScreen(ST7735_BLACK);
  tft.fillRect(0, 15, 160, 25, ST7735_BLUE);
  tft.setTextColor(ST7735_WHITE);
  tft.setTextSize(1);
  tft.setCursor(20, 23);
  tft.print("Your COMMAND DECK");

  tft.setCursor(55, 65);
  tft.setTextColor(ST7735_BLUE);
  tft.print("Welcome!!");
  tft.setCursor(52, 80);
  tft.print("Press [OK]");
  tft.setCursor(40, 95);
  tft.print("to Access Menu");
}

void renderMainMenu() {
  tft.fillScreen(ST7735_BLACK);
  tft.fillRect(0, 0, 160, 18, ST7735_NAVY);
  tft.setTextColor(ST7735_WHITE);
  tft.setCursor(35, 5);
  tft.print("SYSTEM MAIN MENU");

  int yOffset = 30;
  for (int i = 0; i < mainMenuItemCount; i++) {
    if (i == currentSelection) {
      tft.fillRect(4, yOffset - 2, 152, 18, ST7735_DARKGREEN);
      tft.drawRect(4, yOffset - 2, 152, 18, ST7735_BLUE);
      tft.setTextColor(ST7735_WHITE);
    } else {
      tft.setTextColor(ST7735_CYAN);
    }
    tft.setCursor(10, yOffset + 3);
    tft.print(mainMenu[i]);
    yOffset += 24;
  }
}

void renderFetchingScreen(const char* title) {
  tft.fillScreen(ST7735_BLACK);
  tft.fillRect(0, 0, 160, 18, ST7735_BLUE);
  tft.setTextColor(ST7735_WHITE);
  tft.setCursor(10, 5);
  tft.print(title);

  tft.setCursor(20, 60);
  tft.setTextColor(ST7735_CYAN);
  tft.print("Fetching from PC...");
}

void renderDynamicList(const char* title, String items[], int count) {
  tft.fillScreen(ST7735_BLACK);
  tft.fillRect(0, 0, 160, 18, ST7735_BLUE);
  tft.setTextColor(ST7735_WHITE);
  tft.setCursor(10, 5);
  tft.print(title);

  int visibleCount = 5;
  int startIdx = max(0, currentSelection - (visibleCount / 2));
  if (startIdx + visibleCount > count) {
    startIdx = max(0, count - visibleCount);
  }

  int yOffset = 25;
  for (int i = startIdx; i < startIdx + visibleCount && i < count; i++) {
    if (i == currentSelection) {
      tft.fillRect(4, yOffset - 2, 152, 16, ST7735_NAVY);
      tft.drawRect(4, yOffset - 2, 152, 16, ST7735_CYAN);
      tft.setTextColor(ST7735_YELLOW);
    } else {
      tft.setTextColor(ST7735_WHITE);
    }
    tft.setCursor(10, yOffset + 2);
    tft.print(items[i]);
    yOffset += 18;
  }
}

void renderTelemetryInit() {
  tft.fillScreen(ST7735_BLACK);
  tft.fillRect(0, 0, 160, 18, ST7735_DARKGREEN);
  tft.setTextColor(ST7735_WHITE);
  tft.setCursor(22, 5);
  tft.print("PC TELEMETRY STATS");

  tft.setCursor(10, 30);
  tft.print("Connecting...");
  tft.setCursor(10, 112);
  tft.setTextColor(ST7735_DARKGRAY);
  tft.print("[SELECT] -> Main Menu");
}

void updateTelemetryUI(int cpu, int ram) {
  tft.fillRect(0, 22, 160, 85, ST7735_BLACK);

  tft.setTextColor(ST7735_WHITE);
  tft.setCursor(10, 28);
  tft.printf("CPU Usage: %d%%", cpu);
  tft.drawRect(10, 40, 140, 12, ST7735_WHITE);
  int cpuWidth = map(constrain(cpu, 0, 100), 0, 100, 0, 136);
  uint16_t cpuColor = (cpu > 80) ? ST7735_RED : (cpu > 50) ? ST7735_YELLOW : ST7735_GREEN;
  tft.fillRect(12, 42, cpuWidth, 8, cpuColor);

  tft.setCursor(10, 63);
  tft.printf("RAM Usage: %d%%", ram);
  tft.drawRect(10, 75, 140, 12, ST7735_WHITE);
  int ramWidth = map(constrain(ram, 0, 100), 0, 100, 0, 136);
  uint16_t ramColor = (ram > 80) ? ST7735_RED : (ram > 50) ? ST7735_YELLOW : ST7735_GREEN;
  tft.fillRect(12, 77, ramWidth, 8, ramColor);
}