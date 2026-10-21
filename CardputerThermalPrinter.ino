#include <M5Cardputer.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <vector>
#include <Arduino.h>


// ==========================================
//           USER CONFIGURATION
// ==========================================
#define CARDPUTER_TEXT_SIZE_WIDTH 1.2
#define CARDPUTER_TEXT_SIZE_HEIGHT 1.4
#define CARDPUTER_WRAP_LIMIT 32
#define PRINTER_WRAP_LIMIT 32

// Official hardware SPI pins for the Cardputer SD Slot
#define SD_SPI_SCK 40
#define SD_SPI_MISO 39
#define SD_SPI_MOSI 14
#define SD_SPI_CS 12
#define BANNER_CHAR "\xDB"
// ==========================================

static BLEUUID serviceUUID("000018f0-0000-1000-8000-00805f9b34fb");
static BLEUUID charUUID("00002af1-0000-1000-8000-00805f9b34fb");

static BLERemoteCharacteristic* pRemoteCharacteristic;
static BLEAdvertisedDevice* myDevice = nullptr;
static boolean connected = false;

std::vector<BLEAdvertisedDevice> foundDevices;
int selectedIndex = 0;

enum AppState {
  STATE_INIT_SCAN,
  STATE_BLE_MENU,
  STATE_MAIN_MENU,
  STATE_TYPEWRITER,
  STATE_BANNER,
  STATE_SD_BROWSER
};
AppState currentState = STATE_INIT_SCAN;

int mainMenuIndex = 0;
String bannerInput;
uint8_t bannerLineCount = 0;

// C-Style Fixed Terminal Memory Grid
const int MAX_LINES = 9;
const int MAX_COLS = 33;
char terminalGrid[MAX_LINES][MAX_COLS];
int lineCount = 0;

char screenBuffer[MAX_COLS];
int screenIdx = 0;
char printerBuffer[MAX_COLS];
int printerIdx = 0;

// SD Browser Data Arrays
std::vector<String> fileList;
std::vector<bool> isDirectoryList;
int sdSelectedIndex = 0;
bool sdCardReady = false;
String currentFolderPath = "/";

class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) {
    if (advertisedDevice.haveName() && advertisedDevice.getName().length() > 0) {
      for (auto& d : foundDevices) {
        if (d.getAddress().toString() == advertisedDevice.getAddress().toString()) return;
      }
      foundDevices.push_back(advertisedDevice);
    }
  }
};

bool connectToServer() {
  if (myDevice == nullptr) return false;
  BLEClient* pClient = BLEDevice::createClient();
  if (!pClient->connect(myDevice)) return false;

  BLERemoteService* pRemoteService = pClient->getService(serviceUUID);
  if (pRemoteService == nullptr) {
    pClient->disconnect();
    return false;
  }

  pRemoteCharacteristic = pRemoteService->getCharacteristic(charUUID);
  if (pRemoteCharacteristic == nullptr) {
    pClient->disconnect();
    return false;
  }

  connected = true;
  return true;
}

void sendPrinterBytes(const uint8_t* data, size_t len) {
  if (connected && pRemoteCharacteristic) {
    size_t offset = 0;
    const size_t chunkSize = 20;
    while (offset < len) {
      size_t toSend = len - offset;
      if (toSend > chunkSize) toSend = chunkSize;
      pRemoteCharacteristic->writeValue((uint8_t*)(data + offset), toSend, false);
      offset += toSend;
      delay(20);
    }
  }
}

void pushLineToHistory() {
  if (lineCount < MAX_LINES) {
    strncpy(terminalGrid[lineCount], screenBuffer, MAX_COLS);
    lineCount++;
  } else {
    for (int i = 0; i < MAX_LINES - 1; i++) {
      strncpy(terminalGrid[i], terminalGrid[i + 1], MAX_COLS);
    }
    strncpy(terminalGrid[MAX_LINES - 1], screenBuffer, MAX_COLS);
  }
}

void drawBleMenu() {
  M5Cardputer.Display.clear();
  M5Cardputer.Display.setCursor(0, 0);
  M5Cardputer.Display.setTextColor(YELLOW);
  M5Cardputer.Display.println("Searching devices...");
  M5Cardputer.Display.setTextColor(WHITE);
  for (int i = 0; i < CARDPUTER_WRAP_LIMIT; i++) M5Cardputer.Display.print("-");
  M5Cardputer.Display.println();

  if (foundDevices.empty()) {
    M5Cardputer.Display.setTextColor(RED);
    M5Cardputer.Display.println("No printers found.");
    return;
  }

  int start = max(0, selectedIndex - 2);
  int end = min((int)foundDevices.size(), start + 4);

  for (int i = start; i < end; i++) {
    if (i == selectedIndex) {
      M5Cardputer.Display.setTextColor(GREEN);
      M5Cardputer.Display.print("> ");
    } else {
      M5Cardputer.Display.setTextColor(WHITE);
      M5Cardputer.Display.print("  ");
    }
    M5Cardputer.Display.println(foundDevices[i].getName().c_str());
  }
}

void drawMainMenu() {
  M5Cardputer.Display.clear();
  M5Cardputer.Display.setCursor(0, 0);
  M5Cardputer.Display.setTextColor(GREEN);
  M5Cardputer.Display.println("PRINTER TERMINAL ON");
  M5Cardputer.Display.setTextColor(WHITE);
  for (int i = 0; i < CARDPUTER_WRAP_LIMIT; i++) M5Cardputer.Display.print("-");
  M5Cardputer.Display.println("\nSelect Application Mode:\n");

  const char* menuItems[] = {
    "Typewriter Mode",
    "Print a banner",
    "Print From SD Card"
  };

  for (int i = 0; i < 3; ++i) {
    M5Cardputer.Display.setTextColor(mainMenuIndex == i ? GREEN : WHITE);
    M5Cardputer.Display.printf("%s%s\n",
                               mainMenuIndex == i ? " >" : "  ", menuItems[i]);
  }
  M5Cardputer.Display.setTextColor(0xBBBBBB);
  M5Cardputer.Display.setCursor(0, 115);
  M5Cardputer.Display.print("[Navigate ';'/'.' | Go: Enter]");
}




// CRITICAL REWRITE: Isolated extraction algorithm reads directories safely
void readSDFileSystem(String path) {
  fileList.clear();
  isDirectoryList.clear();
  sdSelectedIndex = 0;
  if (!sdCardReady) return;

  if (path != "/") {
    fileList.push_back(".. [Go Back]");
    isDirectoryList.push_back(true);
  }

  // Enforce consistent path formatting bounds
  String targetPath = path;
  if (targetPath.length() > 1 && targetPath.endsWith("/")) {
    targetPath = targetPath.substring(0, targetPath.length() - 1);
  }

  File root = SD.open(targetPath);
  if (!root || !root.isDirectory()) return;

  root.rewindDirectory();

  // Create an explicit storage vector block to hold data points safely
  // PASS 1: Find and collect all directories first
  root.rewindDirectory();
  while (true) {
    File file = root.openNextFile();
    if (!file) break;

    String fullPath = String(file.name());
    String cleanName = fullPath.substring(fullPath.lastIndexOf("/") + 1);

    if (file.isDirectory() && cleanName.length() > 0 && !cleanName.startsWith("._") && !cleanName.startsWith(".")) {
      fileList.push_back("/" + cleanName + "/");
      isDirectoryList.push_back(true);
    }
    file.close();
  }

  // PASS 2: Find and append all normal data files right under folders
  root.rewindDirectory();
  while (true) {
    File file = root.openNextFile();
    if (!file) break;

    String fullPath = String(file.name());
    String cleanName = fullPath.substring(fullPath.lastIndexOf("/") + 1);

    if (!file.isDirectory() && cleanName.length() > 0 && !cleanName.startsWith("._") && !cleanName.startsWith(".")) {
      fileList.push_back(cleanName);
      isDirectoryList.push_back(false);
    }
    file.close();
  }

  root.close();
}

void drawSDBrowser() {
  M5Cardputer.Display.clear();
  M5Cardputer.Display.setCursor(0, 0);
  M5Cardputer.Display.setTextColor(CYAN);
  M5Cardputer.Display.print("Dir: ");
  M5Cardputer.Display.println(currentFolderPath);
  M5Cardputer.Display.setTextColor(WHITE);
  for (int i = 0; i < CARDPUTER_WRAP_LIMIT; i++) M5Cardputer.Display.print("-");
  M5Cardputer.Display.println();

  if (!sdCardReady) {
    M5Cardputer.Display.setTextColor(RED);
    M5Cardputer.Display.println("SD Card Failed.");
    return;
  }

  if (fileList.empty()) {
    M5Cardputer.Display.setTextColor(YELLOW);
    M5Cardputer.Display.println("No files found.");
    return;
  }

  int start = max(0, sdSelectedIndex - 5);
  int end = min((int)fileList.size(), start + 7);

  for (int i = start; i < end; i++) {
    String entryName = fileList[i];
    bool isDir = isDirectoryList[i];
    bool isTxt = entryName.endsWith(".txt") || entryName.endsWith(".TXT");

    if (i == sdSelectedIndex) {
      M5Cardputer.Display.print("> ");
      if (isDir) {
        M5Cardputer.Display.setTextColor(YELLOW);
      } else {
        M5Cardputer.Display.setTextColor(isTxt ? GREEN : 0x777777);
      }
    } else {
      M5Cardputer.Display.print("  ");
      if (isDir) {
        M5Cardputer.Display.setTextColor(0xDDAA00);
      } else {
        M5Cardputer.Display.setTextColor(isTxt ? WHITE : 0x444444);
      }
    }
    M5Cardputer.Display.println(entryName);
  }

  M5Cardputer.Display.setTextColor(0xBBBBBB);
  M5Cardputer.Display.setCursor(0, 115);
  M5Cardputer.Display.print("[Enter: Open/Print | Del: Back]");
}

void printTextFile(String filePath) {
  if (!sdCardReady) return;

  M5Cardputer.Display.clear();
  M5Cardputer.Display.setCursor(0, 0);
  M5Cardputer.Display.setTextColor(YELLOW);
  M5Cardputer.Display.println("Parsing File...");

  File file = SD.open(filePath);
  if (!file) {
    M5Cardputer.Display.setTextColor(RED);
    M5Cardputer.Display.println("Error launching file.");
    delay(2000);
    return;
  }

  char streamLine[MAX_COLS];
  int sIdx = 0;

  while (file.available()) {
    char c = file.read();

    if (c == '\n' || sIdx >= (PRINTER_WRAP_LIMIT - 1)) {
      streamLine[sIdx++] = '\n';
      sendPrinterBytes((uint8_t*)streamLine, sIdx);
      memset(streamLine, 0, sizeof(streamLine));
      sIdx = 0;
    } else if (c != '\r') {
      streamLine[sIdx++] = c;
    }
  }

  if (sIdx > 0) {
    streamLine[sIdx++] = '\n';
    sendPrinterBytes((uint8_t*)streamLine, sIdx);
  }

  uint8_t postFeed[] = { 0x0A, 0x0A, 0x0A };
  sendPrinterBytes(postFeed, 3);

  file.close();
  M5Cardputer.Display.setTextColor(GREEN);
  M5Cardputer.Display.println("Print Complete!");
  delay(1500);
}

void refreshTerminalDisplay() {
  M5Cardputer.Display.clear();
  M5Cardputer.Display.setCursor(0, 0);
  M5Cardputer.Display.setTextColor(GREEN);
  M5Cardputer.Display.println("Live Typewriter:");
  M5Cardputer.Display.setTextColor(WHITE);
  for (int i = 0; i < CARDPUTER_WRAP_LIMIT; i++) M5Cardputer.Display.print("-");
  M5Cardputer.Display.println();

  for (int i = 0; i < lineCount; i++) {
    M5Cardputer.Display.println(terminalGrid[i]);
  }
  M5Cardputer.Display.print("> ");
  M5Cardputer.Display.print(screenBuffer);
}


// ============================================================
// 8x8 ASCII bitmap font
// Printable ASCII characters: 0x20 (' ') through 0x7E ('~')
// ============================================================
//
// Each character occupies 8 bytes.
// Each byte represents one row, with the MSB on the left.
//
// The complete table should contain 95 * 8 = 760 bytes.
//
const uint8_t FONT_8X8[95][8] PROGMEM = {
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },  // Space (0x20)
  { 0x18, 0x18, 0x18, 0x18, 0x00, 0x00, 0x18, 0x00 },  // !
  { 0x66, 0x66, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00 },  // "
  { 0x24, 0x24, 0x7E, 0x24, 0x7E, 0x24, 0x24, 0x00 },  // #
  { 0x00, 0x1C, 0x3A, 0x1C, 0x2E, 0x1C, 0x00, 0x00 },  // $ ... so on
  { 0x00, 0x62, 0x64, 0x08, 0x10, 0x26, 0x46, 0x00 },
  { 0x38, 0x6C, 0x38, 0x74, 0x6C, 0x3A, 0x47, 0x00 },
  { 0x18, 0x18, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00 },
  { 0x0C, 0x18, 0x30, 0x30, 0x30, 0x18, 0x0C, 0x00 },
  { 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x18, 0x30, 0x00 },
  { 0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00 },
  { 0x00, 0x18, 0x18, 0x7E, 0x18, 0x18, 0x00, 0x00 },
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x08 },
  { 0x00, 0x00, 0x00, 0x3E, 0x00, 0x00, 0x00, 0x00 },
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18 },
  { 0x00, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x00 },
  { 0x3C, 0x66, 0x6E, 0x76, 0x66, 0x66, 0x3C, 0x00 },
  { 0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00 },
  { 0x3C, 0x66, 0x06, 0x1C, 0x30, 0x62, 0x7E, 0x00 },
  { 0x3C, 0x66, 0x06, 0x1C, 0x06, 0x66, 0x3C, 0x00 },
  { 0x06, 0x0E, 0x1E, 0x36, 0x7E, 0x06, 0x06, 0x00 },
  { 0x7E, 0x60, 0x7C, 0x06, 0x06, 0x66, 0x3C, 0x00 },
  { 0x1C, 0x30, 0x60, 0x7C, 0x66, 0x66, 0x3C, 0x00 },
  { 0x7E, 0x06, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x00 },
  { 0x3C, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x3C, 0x00 },
  { 0x3C, 0x66, 0x66, 0x3E, 0x06, 0x0C, 0x38, 0x00 },
  { 0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00, 0x00 },
  { 0x00, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x08 },
  { 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x00, 0x00, 0x00 },
  { 0x00, 0x00, 0x7E, 0x00, 0x7E, 0x00, 0x00, 0x00 },
  { 0x30, 0x18, 0x0C, 0x18, 0x30, 0x00, 0x00, 0x00 },
  { 0x3C, 0x66, 0x06, 0x0C, 0x18, 0x00, 0x18, 0x00 },
  { 0x3C, 0x42, 0x5E, 0x56, 0x56, 0x40, 0x3C, 0x00 },
  { 0x18, 0x3C, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00 },
  { 0x7C, 0x66, 0x66, 0x7C, 0x66, 0x66, 0x7C, 0x00 },
  { 0x3C, 0x66, 0x60, 0x60, 0x60, 0x66, 0x3C, 0x00 },
  { 0x78, 0x6C, 0x66, 0x66, 0x66, 0x6C, 0x78, 0x00 },
  { 0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x7E, 0x00 },
  { 0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x60, 0x00 },
  { 0x3C, 0x66, 0x60, 0x6E, 0x66, 0x66, 0x3A, 0x00 },
  { 0x66, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00 },
  { 0x3E, 0x08, 0x08, 0x08, 0x08, 0x08, 0x3E, 0x00 },
  { 0x1E, 0x06, 0x06, 0x06, 0x06, 0x66, 0x3C, 0x00 },
  { 0x66, 0x6C, 0x78, 0x70, 0x78, 0x6C, 0x66, 0x00 },
  { 0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7E, 0x00 },
  { 0x63, 0x77, 0x7F, 0x6B, 0x63, 0x63, 0x63, 0x00 },
  { 0x66, 0x76, 0x7E, 0x6E, 0x66, 0x66, 0x66, 0x00 },
  { 0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00 },
  { 0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x60, 0x00 },
  { 0x3C, 0x66, 0x66, 0x66, 0x6E, 0x3C, 0x0E, 0x00 },
  { 0x7C, 0x66, 0x66, 0x7C, 0x6C, 0x66, 0x66, 0x00 },
  { 0x3C, 0x66, 0x30, 0x18, 0x0C, 0x66, 0x3C, 0x00 },
  { 0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00 },
  { 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00 },
  { 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00 },
  { 0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00 },
  { 0x66, 0x66, 0x3C, 0x18, 0x3C, 0x66, 0x66, 0x00 },
  { 0x66, 0x66, 0x66, 0x3C, 0x18, 0x18, 0x18, 0x00 },
  { 0x7E, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x7E, 0x00 },
  { 0x3C, 0x30, 0x30, 0x30, 0x30, 0x30, 0x3C, 0x00 },
  { 0x00, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x00 },
  { 0x3C, 0x06, 0x06, 0x06, 0x06, 0x06, 0x3C, 0x00 },
  { 0x18, 0x3C, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00 },
  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF },
  { 0x30, 0x30, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00 },
  { 0x00, 0x00, 0x3C, 0x06, 0x3E, 0x66, 0x3B, 0x00 },
  { 0x60, 0x60, 0x7C, 0x66, 0x66, 0x66, 0x7C, 0x00 },
  { 0x00, 0x00, 0x3C, 0x60, 0x60, 0x66, 0x3C, 0x00 },
  { 0x06, 0x06, 0x3E, 0x66, 0x66, 0x66, 0x3E, 0x00 },
  { 0x00, 0x00, 0x3C, 0x66, 0x7E, 0x60, 0x3C, 0x00 },
  { 0x1C, 0x30, 0x78, 0x30, 0x30, 0x30, 0x30, 0x00 },
  { 0x00, 0x00, 0x3B, 0x66, 0x66, 0x3E, 0x06, 0x3C },
  { 0x60, 0x60, 0x7C, 0x66, 0x66, 0x66, 0x66, 0x00 },
  { 0x18, 0x00, 0x38, 0x18, 0x18, 0x18, 0x3C, 0x00 },
  { 0x06, 0x00, 0x0E, 0x06, 0x06, 0x06, 0x06, 0x3C },
  { 0x60, 0x60, 0x66, 0x6C, 0x78, 0x6C, 0x66, 0x00 },
  { 0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00 },
  { 0x00, 0x00, 0x66, 0x7F, 0xDB, 0xDB, 0xDB, 0x00 },
  { 0x00, 0x00, 0x7C, 0x66, 0x66, 0x66, 0x66, 0x00 },
  { 0x00, 0x00, 0x3C, 0x66, 0x66, 0x66, 0x3C, 0x00 },
  { 0x00, 0x00, 0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60 },
  { 0x00, 0x00, 0x3E, 0x66, 0x66, 0x3E, 0x06, 0x06 },
  { 0x00, 0x00, 0x7C, 0x66, 0x60, 0x60, 0x60, 0x00 },
  { 0x00, 0x00, 0x3E, 0x60, 0x3C, 0x02, 0x7C, 0x00 },
  { 0x18, 0x18, 0x7C, 0x18, 0x18, 0x18, 0x1C, 0x00 },
  { 0x00, 0x00, 0x66, 0x66, 0x66, 0x66, 0x3B, 0x00 },
  { 0x00, 0x00, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00 },
  { 0x00, 0x00, 0xDB, 0xDB, 0xDB, 0x7F, 0x24, 0x00 },
  { 0x00, 0x00, 0x66, 0x3C, 0x18, 0x3C, 0x66, 0x00 },
  { 0x00, 0x00, 0x66, 0x66, 0x66, 0x3E, 0x06, 0x3C },
  { 0x00, 0x00, 0x7E, 0x0C, 0x18, 0x30, 0x7E, 0x00 },
  { 0x0E, 0x18, 0x18, 0x70, 0x18, 0x18, 0x0E, 0x00 },
  { 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00 },
  { 0x70, 0x18, 0x18, 0x0E, 0x18, 0x18, 0x70, 0x00 },
  { 0x76, 0xDC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
};

// ============================================================
// Generated 32-bit native font
// ============================================================
//
// Each ASCII character has 8 uint32_t values.
// Each uint32_t represents one generated column.
//
// 95 characters * 8 columns * 4 bytes = 3040 bytes.
//
uint32_t FONT_32X32_NATIVE[95][8];

// ============================================================
// Read a character from the 8x8 font
// ============================================================

uint8_t getFontPixelRow(char character, uint8_t row) {
  if (character < 0x20 || character > 0x7E) {
    character = ' ';
  }

  uint8_t characterIndex = character - 0x20;

  return pgm_read_byte(
    &FONT_8X8[characterIndex][row]);
}

// ============================================================
// Generate the native 32x32 font
// Equivalent to generate_native_32x32_rotated_font()
// ============================================================

void generateNativeFont() {
  for (uint16_t characterIndex = 0;
       characterIndex < 95;
       characterIndex++) {

    for (uint8_t column = 0; column < 8; column++) {
      uint32_t lineBits = 0;

      for (int8_t row = 7; row >= 0; row--) {
        uint8_t sourceRow = pgm_read_byte(
          &FONT_8X8[characterIndex][row]);
        bool isBitOn =
          (sourceRow >> (7 - column)) & 1;
        if (isBitOn) {
          uint8_t position = row * 4;
          lineBits |= (uint32_t)0xF << position;
        }
      }
      FONT_32X32_NATIVE[characterIndex][column] = lineBits;
    }
  }
}

// ============================================================
// Print the two-line sideways banner
// Equivalent to Option A
// ============================================================

void printTwoLineBanner(const char* inputString) {
  const uint8_t PRINTER_WIDTH = 32;

  // Find the first newline.
  const char* newline = strchr(inputString, '\n');

  if (newline == nullptr) {
    return;
  }

  // Length of the first line.
  size_t line1Length = newline - inputString;

  // Length of the second line.
  const char* line2 = newline + 1;
  size_t line2Length = strlen(line2);

  if (line2Length == 0) {
    printNativeBanner(inputString);  // One line banner
    return;
  }

  size_t maxLength = max(line1Length, line2Length);

  for (size_t characterIndex = 0;
       characterIndex < maxLength;
       characterIndex++) {

    char character1 =
      characterIndex < line1Length
        ? inputString[characterIndex]
        : ' ';

    char character2 =
      characterIndex < line2Length
        ? line2[characterIndex]
        : ' ';

    for (int8_t column = 7; column >= 0; column--) {

      // Python iterates columns 0..7, so preserve that
      // order here.
      uint8_t actualColumn = 7 - column;

      for (int8_t row = 7; row >= 0; row--) {

        uint8_t value2 =
          getFontPixelRow(character2, row);

        if ((value2 >> (7 - actualColumn)) & 1) {
          // Serial.print(F("██"));
          sendPrinterBytes((const uint8_t*)BANNER_CHAR BANNER_CHAR, 2);
        } else {
          sendPrinterBytes((const uint8_t*)"  ", 2);
        }
      }

      for (int8_t row = 7; row >= 0; row--) {

        uint8_t value1 =
          getFontPixelRow(character1, row);

        if ((value1 >> (7 - actualColumn)) & 1) {
          // Serial.print(F("██"));
          sendPrinterBytes((const uint8_t*)BANNER_CHAR BANNER_CHAR, 2);
        } else {
          sendPrinterBytes((const uint8_t*)"  ", 2);
        }
      }

      // sendPrinterBytes((const uint8_t*)"\n", 1);
    }

    // Character spacing.
    // for (uint8_t i = 0; i < PRINTER_WIDTH; i++) {
    //     sendPrinterBytes((const uint8_t*)"                                ", 32);
    // }

    // sendPrinterBytes((const uint8_t*)"\n", 1);
  }
}

// ============================================================
// Print the 32x32 native banner
// Equivalent to Option B
// ============================================================

void printNativeBanner(const char* inputString) {
  const uint8_t PRINTER_WIDTH = 32;

  for (size_t characterIndex = 0; inputString[characterIndex] != '\0'; characterIndex++) {
    char character = inputString[characterIndex];
    if (character < 0x20 || character > 0x7E) {
      character = ' ';
    }

    uint8_t fontIndex = character - 0x20;

    for (uint8_t row = 0; row < 8; row++) {
      uint32_t lineBits = FONT_32X32_NATIVE[fontIndex][row];
      for (int8_t bit = 31; bit >= 0; bit--) {
        bool isBitOn = (lineBits >> bit) & 1UL;
        sendPrinterBytes((const uint8_t*)(isBitOn ? BANNER_CHAR : " "), 1);
        // Serial.print(isBitOn ? '█' : ' ');
      }
      // sendPrinterBytes((const uint8_t*)"\n", 1);
    }

    // Controlled character spacing.
    // for (uint8_t i = 0; i < PRINTER_WIDTH; i++) {
    //     sendPrinterBytes((const uint8_t*)"                                ", 32);
    // }

    // sendPrinterBytes((const uint8_t*)"\n", 1);
  }
}

// ============================================================
// Main banner dispatcher
// Equivalent to print_thermal_banner()
// ============================================================

void printThermalBanner(const char* inputString) {
  // if (strchr(inputString, '\n') != nullptr) {
  printTwoLineBanner(inputString);
  // } else {
  // printNativeBanner(inputString);
  // }
}



void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);

  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setTextSize(CARDPUTER_TEXT_SIZE_WIDTH, CARDPUTER_TEXT_SIZE_HEIGHT);
  M5Cardputer.Display.setTextScroll(false);
  M5Cardputer.Display.clear();
  M5Cardputer.Display.setCursor(0, 0);

  M5Cardputer.Display.setTextColor(YELLOW);
  M5Cardputer.Display.println("System Startup...");
  M5Cardputer.Display.println("Searching devices...");

  SPI.begin(SD_SPI_SCK, SD_SPI_MISO, SD_SPI_MOSI, SD_SPI_CS);

  if (SD.begin(SD_SPI_CS, SPI, 25000000, "/sd", 1)) {
    sdCardReady = true;
  }

  memset(screenBuffer, 0, sizeof(screenBuffer));
  memset(printerBuffer, 0, sizeof(printerBuffer));
  for (int i = 0; i < MAX_LINES; i++) memset(terminalGrid[i], 0, MAX_COLS);

  Serial.begin(115200);

  BLEDevice::init("M5Cardputer");
  BLEScan* pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setInterval(1349);
  pBLEScan->setWindow(449);
  pBLEScan->setActiveScan(true);

  pBLEScan->start(5, false);
  currentState = STATE_BLE_MENU;
  drawBleMenu();


  //testing
  Serial.begin(115200);

  // Generate the font once during startup.
  generateNativeFont();
  printThermalBanner("Hello");
  printThermalBanner("HELLO\nWORLD");
}

void loop() {
  M5Cardputer.update();

  if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
    auto status = M5Cardputer.Keyboard.keysState();

    switch (currentState) {

      case STATE_BLE_MENU:
        for (auto i : status.word) {
          if (i == ';') {
            if (selectedIndex > 0) selectedIndex--;
            drawBleMenu();
          }
          if (i == '.') {
            if (selectedIndex < (int)foundDevices.size() - 1) selectedIndex++;
            drawBleMenu();
          }
        }
        if (status.enter && !foundDevices.empty()) {
          M5Cardputer.Display.clear();
          M5Cardputer.Display.setCursor(0, 0);
          M5Cardputer.Display.setTextColor(YELLOW);
          M5Cardputer.Display.println("Connecting...");

          myDevice = &foundDevices[selectedIndex];
          if (connectToServer()) {
            uint8_t initCmd[] = { 0x1B, 0x40 };
            sendPrinterBytes(initCmd, sizeof(initCmd));

            currentState = STATE_MAIN_MENU;
            drawMainMenu();
          } else {
            M5Cardputer.Display.setTextColor(RED);
            M5Cardputer.Display.println("Connection failed.");
            delay(1500);
            drawBleMenu();
          }
        }
        break;

      case STATE_MAIN_MENU:
        for (auto i : status.word) {
          if (i == ';') {
            mainMenuIndex = (mainMenuIndex + 2) % 3;
            drawMainMenu();
          }
          if (i == '.') {
            mainMenuIndex = (mainMenuIndex + 1) % 3;
            drawMainMenu();
          }
        }
        if (status.enter) {
          if (mainMenuIndex == 0) {
            currentState = STATE_TYPEWRITER;
            refreshTerminalDisplay();
          } else if (mainMenuIndex == 1) {
            bannerInput = "";
            bannerLineCount = 0;
            currentState = STATE_BANNER;
            M5Cardputer.Display.clear();
            M5Cardputer.Display.setCursor(0, 0);
            M5Cardputer.Display.println("Banner (max 2 lines):");
          } else {
            currentState = STATE_SD_BROWSER;
            currentFolderPath = "/";
            readSDFileSystem(currentFolderPath);
            drawSDBrowser();
          }
        }
        break;

      case STATE_BANNER:
        for (auto c : status.word) {
          if (c == '\n' || c == '\r') continue;

          if (c >= 0x20 && c <= 0x7E && bannerInput.length() < 65) {
            bannerInput += c;
            M5Cardputer.Display.print(c);
          }
        }

        if (status.enter) {
          if (bannerLineCount == 0) {
            bannerInput += '\n';
            bannerLineCount = 1;
            M5Cardputer.Display.println();
          } else {
            printThermalBanner(bannerInput.c_str());
            bannerInput = "";
            bannerLineCount = 0;
            currentState = STATE_MAIN_MENU;
            drawMainMenu();
          }
        }

        if (status.del) {
          if (bannerInput.length() > 0) {
            bannerInput.remove(bannerInput.length() - 1);
            M5Cardputer.Display.clear();
            M5Cardputer.Display.setCursor(0, 0);
            M5Cardputer.Display.println("Banner (max 2 lines):");
            M5Cardputer.Display.print(bannerInput);
          } else {
            currentState = STATE_MAIN_MENU;
            drawMainMenu();
          }
        }
        break;

      case STATE_TYPEWRITER:
        for (auto i : status.word) {
          if (screenIdx < CARDPUTER_WRAP_LIMIT && printerIdx < PRINTER_WRAP_LIMIT) {
            screenBuffer[screenIdx++] = i;
            screenBuffer[screenIdx] = '\0';
            printerBuffer[printerIdx++] = i;
            printerBuffer[printerIdx] = '\0';
          }
          if (printerIdx >= PRINTER_WRAP_LIMIT) {
            sendPrinterBytes((uint8_t*)printerBuffer, printerIdx);
            uint8_t feedCmd[] = { 0x0A };
            sendPrinterBytes(feedCmd, 1);
            memset(printerBuffer, 0, sizeof(printerBuffer));
            printerIdx = 0;
          }
          if (screenIdx >= CARDPUTER_WRAP_LIMIT) {
            pushLineToHistory();
            memset(screenBuffer, 0, sizeof(screenBuffer));
            screenIdx = 0;
          }
        }
        if (status.enter) {
          if (printerIdx > 0) sendPrinterBytes((uint8_t*)printerBuffer, printerIdx);
          uint8_t feedCmd[] = { 0x0A };
          sendPrinterBytes(feedCmd, 1);

          pushLineToHistory();
          memset(screenBuffer, 0, sizeof(screenBuffer));
          memset(printerBuffer, 0, sizeof(printerBuffer));
          screenIdx = 0;
          printerIdx = 0;
        }
        if (status.del) {
          if (screenIdx > 0 && printerIdx > 0) {
            screenBuffer[--screenIdx] = '\0';
            printerBuffer[--printerIdx] = '\0';
          } else if (screenIdx == 0 && printerIdx == 0 && lineCount == 0) {
            currentState = STATE_MAIN_MENU;
            drawMainMenu();
            break;
          }
        }
        refreshTerminalDisplay();
        break;

      case STATE_SD_BROWSER:
        for (auto i : status.word) {
          if (i == ';') {
            if (sdSelectedIndex > 0) sdSelectedIndex--;
            drawSDBrowser();
          }
          if (i == '.') {
            if (sdSelectedIndex < (int)fileList.size() - 1) sdSelectedIndex++;
            drawSDBrowser();
          }
        }

        if (status.enter && !fileList.empty()) {
          String selectedEntry = fileList[sdSelectedIndex];
          bool isDir = isDirectoryList[sdSelectedIndex];

          if (isDir) {
            if (selectedEntry == ".. [Go Back]") {
              int trailingSlash = currentFolderPath.lastIndexOf('/', currentFolderPath.length() - 2);
              if (trailingSlash >= 0) {
                currentFolderPath = currentFolderPath.substring(0, trailingSlash + 1);
              } else {
                currentFolderPath = "/";
              }
            } else {
              String rawDirName = selectedEntry;
              if (rawDirName.startsWith("/")) rawDirName = rawDirName.substring(1);
              if (rawDirName.endsWith("/")) rawDirName = rawDirName.substring(0, rawDirName.length() - 1);

              currentFolderPath = currentFolderPath + rawDirName + "/";
            }

            readSDFileSystem(currentFolderPath);
            drawSDBrowser();
          } else {
            if (selectedEntry.endsWith(".txt") || selectedEntry.endsWith(".TXT")) {
              String fullPathString = currentFolderPath + selectedEntry;
              printTextFile(fullPathString);
            }
            drawSDBrowser();
          }
        }

        if (status.del) {
          currentState = STATE_MAIN_MENU;
          drawMainMenu();
        }
        break;
    }
  }
  delay(15);
}