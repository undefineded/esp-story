#pragma once
#include "esp_err.h"
#include "route.h"
#include <cstddef>
namespace Storage { class FileReader; }
namespace Host {
// Application lifetime, hardware USB Serial/JTAG. Call once, from app startup.
esp_err_t startUsb();
// Publish once after mounting SD. Reader and its gate must live for the app's
// lifetime; only the USB worker accesses the reader after publication.
esp_err_t attachFiles(Storage::FileReader& files);
// Queue an unsolicited JSON document (an event, not a reply) for the host. Safe
// from any task; the frame is copied and silently dropped if the TX queue is
// full, matching the response backpressure policy.
esp_err_t publishJson(const char* json, std::size_t length);
// True when a host command arrived within the window. Used by the app to let a
// standalone device act on NFC cards without fighting an active workbench.
bool recentlyActive(uint32_t within_ms);
}
