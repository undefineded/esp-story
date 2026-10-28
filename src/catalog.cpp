#include "catalog.h"
#include "storage.h"
#include "story.h"
#include <SD_MMC.h>

namespace {
constexpr const char* INDEX = "/.esp-story-index.bin";
constexpr const char* TEMP = "/.esp-story-index.tmp";
constexpr const char* BACKUP = "/.esp-story-index.bak";
constexpr uint32_t MAGIC = 0x45535049, VERSION = 2;   // v2: 每条曲目多存一个 uuid (文件名散列)

bool transfer(File& file, bool reading, bool apply = true) {
  CatalogArchive archive(file, reading);
  uint32_t magic = MAGIC, version = VERSION;
  return archive.number(magic) && magic == MAGIC
      && archive.number(version) && version == VERSION
      && Storage::index(archive, reading, apply) && Story::index(archive, reading, apply)
      && archive.finish();
}

bool load(const char* path) {
  File file = SD_MMC.open(path, FILE_READ);
  if (!file || !transfer(file, true, false) || !file.seek(0)) return false;
  return transfer(file, true);
}

bool save() {
  File file = SD_MMC.open(TEMP, FILE_WRITE);
  if (!file) return false;
  bool ok = transfer(file, false);
  file.flush();
  file.close();
  // Reopen and validate actual bytes before replacing the published index.
  File verify = SD_MMC.open(TEMP, FILE_READ);
  if (!ok || !verify || !transfer(verify, true, false)) return false;
  verify.close();
  if (SD_MMC.exists(INDEX)) {
    File old = SD_MMC.open(INDEX, FILE_READ);
    bool valid = old && transfer(old, true, false);
    old.close();
    if (valid) {
      if (SD_MMC.exists(BACKUP) && !SD_MMC.remove(BACKUP)) return false;
      if (!SD_MMC.rename(INDEX, BACKUP)) return false;
    } else if (!SD_MMC.remove(INDEX)) return false;
  }
  if (!SD_MMC.rename(TEMP, INDEX)) return false;
  // Keep the previous index for recovery if power is lost during replacement.
  return true;
}
}

namespace Catalog {
Result begin() {
  if (load(INDEX) || load(BACKUP)) {
    Serial.printf("[INDEX] loaded: %d tracks, %d stories\n", Storage::totalCount(), Story::count());
    return Result::Loaded;
  }
  return refresh();
}

Result refresh() {
  if (!Storage::ready() && !Storage::begin()) return Result::ScanFailed;
  if (!Storage::rescan() || !Story::rescan()) {
    // Restore the last valid list if scanning could not finish.
    if (!load(INDEX) && !load(BACKUP)) {
      Storage::clear();
      Story::clear();
    }
    return Result::ScanFailed;
  }
  bool saved = save();
  Serial.printf("[INDEX] refreshed: %d tracks, %d stories; saved=%s\n",
                Storage::totalCount(), Story::count(), saved ? "yes" : "no");
  return saved ? Result::Saved : Result::SaveFailed;
}
}
