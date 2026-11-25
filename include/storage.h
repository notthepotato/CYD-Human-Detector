// SD card: settings (detector/config.txt) and a log of arrivals/departures (detector/log.csv).
// Everything works without a card.
#pragma once
#include "detector.h"

namespace storage {

enum class Status { NoCard, Ready, Error };

// Mounts the card and writes a commented config.txt if there isn't one.
Status begin();
Status status();

// Reads config.txt into `settings`. Returns how many lines were ignored (reasons go to Serial).
int loadConfig(Settings& settings);

// Appends one CSV row (from formatLogRow) to log.csv.
bool appendLog(const char* row);

}  // namespace storage
