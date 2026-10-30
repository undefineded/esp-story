#pragma once
#include "esp_err.h"
namespace Storage { class FileReader; }
namespace Host {
// Application lifetime, hardware USB Serial/JTAG. Call once, from app startup.
esp_err_t startUsb();
// Publish once after mounting SD. Reader and its gate must live for the app's
// lifetime; only the USB worker accesses the reader after publication.
esp_err_t attachFiles(Storage::FileReader& files);
}
