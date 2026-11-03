#pragma once
#include <cstddef>
#include <memory>

#include "media_library.h"
#include "parental_binding.h"
#include "parental_policy.h"
#include "playback.h"

namespace App {

using CatalogPtr = std::shared_ptr<const Media::Catalog>;

// Catalog access. snapshots are shared_ptr so a page can keep one alive while
// a rescan publishes a new one.
CatalogPtr catalog();
Media::Playlist& playlist();
Audio::Playback& playback();
// Storage lease gate for synchronous reads (story LRC, JPEG panels).
Storage::AccessGate& storageGate();

// Loads the v2 index cache; falls back to a full scan (blocking, SD I/O) and
// writes the index back. Returns false when the card is missing or the scan
// failed, leaving the previous catalog untouched.
bool refreshCatalog();

// Plays the current playlist entry. Returns ESP_OK when the command was queued.
bool playCurrent(bool auto_start);
bool stopPlayback();
void pausePlayback(bool paused);
// What the phone should show in evt:state. Set by the page that started the
// media ("music"/"story") and cleared when playback stops.
void setNowPlaying(const char* mode, const char* title);

// Opens the story player for a catalog index (pushes the page).
void openStory(std::size_t index);
// Catalog index of the story currently on the player page, or -1.
void setCurrentStory(int index);
int currentStory();
void setCurrentVideo(int index);
int currentVideo();

// Parental control. A device without a valid PCTL record is provisioned with
// permissive defaults on first boot; every play entry goes through
// playbackAllowed() and the volume through clampVolume().
bool parentalReady();
Persistence::PolicyLock parentalLock();
const char* parentalBindCode();
bool parentalBound();
bool playbackAllowed();
int clampVolume(int value);
bool storyAllowed(std::size_t index);
bool categoryAllowed(int category);
void parentalTick(bool playing);
void parentalSaveIfDirty(uint32_t now_ms);
const char* lockReasonText(Persistence::PolicyLock lock);
uint32_t parentalUsedSeconds();

// Parental settings editing. The page is gated by the PIN: unlockParental()
// marks the session (5 minutes) so re-entering the page does not ask again.
Persistence::ParentalRecord parentalRecord();
bool parentalApply(const Persistence::ParentalRecord& record);
bool parentalHasPin();
bool parentalUnlocked();
bool unlockParental(const char* pin);
void lockParentalSession();
bool parentalSetPin(const char* pin);
void openParentalSettings();

Media::Catalog emptyCatalog();

}
