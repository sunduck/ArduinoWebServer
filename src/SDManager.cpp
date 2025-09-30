#include "SDManager.h"
#include "SoilLogManager.h"

#define SCK  42
#define MISO 12
#define MOSI 13
#define CS   41

bool sdAvailable = false;
static File uploadFile;
static SPIClass spi = SPIClass(HSPI);

void setupSD() {
  spi.begin(SCK, MISO, MOSI, CS);

  // Lower SPI clock to improve stability
  if (!SD.begin(CS, spi, 25000000)) {
    Serial0.println("[ERROR] SD card mount failed");
    sdAvailable = false;
    return;
  }

  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial0.println("[ERROR] No SD card attached");
    sdAvailable = false;
    return;
  }

  sdAvailable = true;
  Serial0.println("[INFO] SD card initialized");

  if (!SD.exists("/uploads")) {
    SD.mkdir("/uploads");
  }
}

void dumpSoilLogsToSD() {
  if (!sdAvailable || logCount == 0) return;

  // filename based on current date
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  char fname[32];
  strftime(fname, sizeof(fname), "/soil_%Y-%m-%d.csv", &timeinfo);
  String path = String(fname);

  File file = SD.open(path.c_str(), FILE_APPEND);
  if (!file) {
    Serial0.println("[ERROR] Failed to open soil log file for writing");
    return;
  }

  // write header only if new file
  if (file.size() == 0) {
    file.println("timestamp,sensor0,sensor1,sensor2,sensor3,watering");
  }

  int idx = (logIndex - logCount + MAX_LOGS) % MAX_LOGS;
  for (int i = 0; i < logCount; i++) {
    SoilLog &entry = soilLogs[(idx + i) % MAX_LOGS];

    // timestamp
    char buf[25];
    struct tm entryTime;
    localtime_r(&entry.timestamp, &entryTime);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &entryTime);

    file.print(buf);
    file.print(",");
    file.print(entry.values[0]);
    file.print(",");
    file.print(entry.values[1]);
    file.print(",");
    file.print(entry.values[2]);
    file.print(",");
    file.print(entry.values[3]);
    file.print(",");

    // watering info (valve:seconds pairs)
    bool first = true;
    for (int v = 0; v < 4; v++) {
      if (entry.watering[v]) {
        if (!first) file.print("|");
        file.print(v);
        file.print(":");
        file.print(entry.wateringTime[v]);
        first = false;
      }
    }
    file.println();
  }

  file.flush();
  file.close();
  Serial0.println("[INFO] Soil logs dumped to " + path);
}

bool saveUploadedFile(const String &filename, size_t index, uint8_t *data, size_t len, bool final) {
  if (!sdAvailable) return false;

  if (index == 0) {
    String path = "/uploads/" + filename;
    uploadFile = SD.open(path.c_str(), FILE_WRITE);
    if (!uploadFile) {
      Serial0.println("[ERROR] Failed to open file for writing: " + path);
      return false;
    }
  }

  if (uploadFile) {
    uploadFile.write(data, len);
    if (final) {
      uploadFile.flush();   // ✅ ensure data is written
      uploadFile.close();   // ✅ properly close file
      Serial0.println("[INFO] Upload finished: " + filename);
    }
    return true;
  }

  return false;
}