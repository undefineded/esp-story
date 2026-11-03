#pragma once
#include <cstddef>
#include <memory>

#include "media_library.h"
#include "playback.h"

namespace App {

using CatalogPtr = std::shared_ptr<const Media::Catalog>;

// Catalog access. snapshots are shared_ptr so a page can keep one alive while
// a rescan publishes a new one.
CatalogPtr catalog();
Media::Playlist& playlist();
Audio::Playback& playback();

// Loads the v2 index cache; falls back to a full scan (blocking, SD I/O) and
// writes the index back. Returns false when the card is missing or the scan
// failed, leaving the previous catalog untouched.
bool refreshCatalog();

// Plays the current playlist entry. Returns ESP_OK when the command was queued.
bool playCurrent(bool auto_start);
bool stopPlayback();
void pausePlayback(bool paused);

Media::Catalog emptyCatalog();

}
