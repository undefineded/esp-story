#pragma once
// Host-route extension point, kept free of esp_err_t so it can be included by
// host tests that compile the protocol router without ESP-IDF.
struct cJSON;
namespace Host {

// Extra command router (sys stats/set, nfc, ...). Called for commands the core
// router does not own; returning true means the command was recognized and
// `response` was filled (set *error for a failure). The feature string is
// advertised in hello so clients can enable the matching UI.
using Route = bool (*)(void* context, struct cJSON* request, const char* command,
                       const char* action, struct cJSON* response, const char** error);

bool attachRoutes(Route route, void* context, const char* feature);

}
