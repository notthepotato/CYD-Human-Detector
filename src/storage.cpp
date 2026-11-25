#include "storage.h"

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

#include "pins.h"

namespace storage {
namespace {

SPIClass gSdSpi(VSPI);
Status gStatus = Status::NoCard;

const char* kDir = "/detector";
const char* kConfigPath = "/detector/config.txt";
const char* kLogPath = "/detector/log.csv";
const char* kOldLogPath = "/detector/log-old.csv";
constexpr size_t kMaxLogBytes = 4 * 1024 * 1024;  // then log.csv becomes log-old.csv

}  // namespace

Status begin() {
  gSdSpi.begin(Pin::SdSck, Pin::SdMiso, Pin::SdMosi, Pin::SdCs);
  if (!SD.begin(Pin::SdCs, gSdSpi, 4000000) || SD.cardType() == CARD_NONE) {
    gStatus = Status::NoCard;
    return gStatus;
  }
  if (!SD.exists(kDir) && !SD.mkdir(kDir)) {
    gStatus = Status::Error;
    return gStatus;
  }
  if (!SD.exists(kConfigPath)) {
    File f = SD.open(kConfigPath, FILE_WRITE);
    if (f) {
      f.print(defaultConfigText());
      f.close();
    }
  }
  gStatus = Status::Ready;
  return gStatus;
}

Status status() { return gStatus; }

int loadConfig(Settings& settings) {
  if (gStatus != Status::Ready) return 0;
  File f = SD.open(kConfigPath, FILE_READ);
  if (!f) return 0;
  int problems = 0;
  int lineNumber = 0;
  char line[128];
  while (f.available()) {
    size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = '\0';
    lineNumber++;
    char why[64] = "";
    if (applyConfigLine(line, settings, why, sizeof(why)) == ConfigLine::Error) {
      problems++;
      Serial.printf("config.txt line %d ignored: %s\n", lineNumber, why);
    }
  }
  f.close();
  return problems;
}

bool appendLog(const char* row) {
  if (gStatus != Status::Ready) return false;

  File existing = SD.open(kLogPath, FILE_READ);
  bool isNew = !existing;
  if (existing) {
    bool full = existing.size() > kMaxLogBytes;
    existing.close();
    if (full) {
      SD.remove(kOldLogPath);
      SD.rename(kLogPath, kOldLogPath);
      isNew = true;
    }
  }

  File f = SD.open(kLogPath, FILE_APPEND);
  if (!f) {
    gStatus = Status::Error;
    return false;
  }
  if (isNew) f.print(logHeader());
  size_t written = f.print(row);
  f.close();  // close every time so pulling the power loses at most one row
  if (written == 0) {
    gStatus = Status::Error;
    return false;
  }
  return true;
}

}  // namespace storage
