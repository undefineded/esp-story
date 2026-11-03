#pragma once
// Host-route extension point, kept free of esp_err_t so it can be included by
// host tests that compile the protocol router without ESP-IDF.
#include <cstddef>
#include <cstdint>

struct cJSON;
namespace Host {

// Extra command router (sys stats/set/ota, nfc, ...). Called for commands the
// core router does not own; returning true means the command was recognized and
// `response` was filled (set *error for a failure). Raw bytes are only provided
// for acts the client marked as binary (sys ota_chunk, fs write_chunk).
using Route = bool (*)(void* context, struct cJSON* request, const char* command,
                       const char* action, struct cJSON* response, const char** error,
                       const uint8_t* raw, std::size_t raw_length);

bool attachRoutes(Route route, void* context, const char* feature);

}
