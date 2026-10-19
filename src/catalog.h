#pragma once
#include <FS.h>
#include "index_io.h"

using CatalogArchive = IndexIO::Archive<fs::File>;

namespace Catalog {
enum class Result { Loaded, Saved, SaveFailed, ScanFailed };
Result begin();                 // load valid index or scan once
Result refresh();               // caller must stop and acknowledge audio first
}
