#pragma once
#include "storage_access.h"
namespace Display {
// UI owner only; hold the storage lease for the entire synchronous JPEG decode.
bool drawStoryImage(Storage::AccessGate& gate,const std::string& root,const std::string& path);
}
