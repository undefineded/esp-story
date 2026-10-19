// Compile production catalog.cpp against an in-memory SD fault injector.
#include "catalog.h"
#include "storage.h"
#include "story.h"
#include <SD_MMC.h>
#include <cassert>
#include <cstdio>

TestSerial Serial;
TestSD SD_MMC;
static uint32_t music = 0, stories = 0, diskMusic = 3, diskStories = 2;
static int scans = 0;
static bool scanFails = false;
namespace Storage {
bool ready() { return true; }
bool begin() { return true; }
bool rescan() { ++scans; music = diskMusic; return !scanFails; }
void clear() { music = 0; }
int totalCount() { return music; }
bool index(CatalogArchive& a, bool reading, bool apply) {
  uint32_t n = music;
  if (!a.number(n) || n > 300) return false;
  if (reading && apply) music = n;
  return true;
}
}
namespace Story {
bool rescan() { ++scans; stories = diskStories; return !scanFails; }
void clear() { stories = 0; }
int count() { return stories; }
bool index(CatalogArchive& a, bool reading, bool apply) {
  uint32_t n = stories;
  if (!a.number(n) || n > 32) return false;
  if (reading && apply) stories = n;
  return true;
}
}

int main() {
  constexpr const char* index = "/.esp-story-index.bin";
  constexpr const char* backup = "/.esp-story-index.bak";
  assert(Catalog::begin() == Catalog::Result::Saved && scans == 2);
  music = stories = 0;
  assert(Catalog::begin() == Catalog::Result::Loaded && scans == 2);
  assert(music == 3 && stories == 2);

  diskMusic = 5;
  assert(Catalog::refresh() == Catalog::Result::Saved);
  assert(SD_MMC.exists(backup) && music == 5);
  auto good = *SD_MMC.files[index];
  (*SD_MMC.files[index])[8] ^= 1; // valid count but wrong checksum
  assert(Catalog::begin() == Catalog::Result::Loaded && music == 3);
  *SD_MMC.files[index] = good;

  // Short writes must not replace the valid index or mutate the scanned list.
  SD_MMC.failWrite = true; diskMusic = 6;
  assert(Catalog::refresh() == Catalog::Result::SaveFailed && music == 6);
  assert(*SD_MMC.files[index] == good);
  SD_MMC.failWrite = false;
  assert(Catalog::begin() == Catalog::Result::Loaded && music == 5);

  // Interrupted replacement recovers the previous generation without scanning.
  SD_MMC.failRename = true;
  assert(Catalog::refresh() == Catalog::Result::SaveFailed);
  assert(!SD_MMC.exists(index) && SD_MMC.exists(backup));
  int before = scans;
  assert(Catalog::begin() == Catalog::Result::Loaded && music == 5 && scans == before);
  SD_MMC.failRename = false;
  assert(Catalog::refresh() == Catalog::Result::Saved && music == 6);

  scanFails = true; diskMusic = 10;
  assert(Catalog::refresh() == Catalog::Result::ScanFailed && music == 6);
  scanFails = false;
  SD_MMC.files.clear();
  diskMusic = diskStories = 0;
  assert(Catalog::begin() == Catalog::Result::Saved);
  before = scans;
  assert(Catalog::begin() == Catalog::Result::Loaded && scans == before);

  // No backup and a truncated primary requires a rebuild.
  SD_MMC.files[index]->resize(9);
  assert(Catalog::begin() == Catalog::Result::Saved && scans == before + 2);

  // Codec round-trip preserves UTF-8 and rejects oversized/corrupt/trailing data.
  auto bytes = std::make_shared<std::vector<uint8_t>>();
  File out(bytes, true, false);
  IndexIO::Archive<File> writer(out, false);
  std::string title = "故事/小小故事盒.mp3";
  assert(writer.text(title) && writer.finish());
  File in(bytes, false, false);
  IndexIO::Archive<File> reader(in, true);
  std::string decoded;
  assert(reader.text(decoded) && reader.finish() && decoded == title);
  bytes->push_back(0);
  File extra(bytes, false, false);
  IndexIO::Archive<File> extraReader(extra, true);
  assert(extraReader.text(decoded) && !extraReader.finish());
  (*bytes)[0] = 0xff; (*bytes)[1] = 0xff;
  File invalid(bytes, false, false);
  IndexIO::Archive<File> invalidReader(invalid, true);
  assert(!invalidReader.text(decoded));
  puts("PASS: cached boot, refresh, checksum, backup recovery, short writes, interrupted rename, scan failure, empty card, UTF-8 and bounds");
}
