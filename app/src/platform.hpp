#pragma once

// What differs between the desktop and Android builds.

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace airlink::platform {

struct Config {
    std::string dataDir;       // empty: the core's per-user default
    std::string downloadDir;   // empty: Downloads/AirLink
    std::string fallbackName;  // used when no device name was saved before
};

// The start configuration, once the platform knows it (on Android the activity tells it).
std::optional<Config> config();

// Lets the user choose files to send. `done` gets local paths (empty if cancelled); it may be
// called on any thread.
void pickFiles(std::function<void(std::vector<std::string>)> done);

// Copies URIs that aren't local files (Android's content://) to local files; `done` gets their
// paths, maybe on another thread.
void stageUris(const std::vector<std::string>& uris, std::function<void(std::vector<std::string>)> done);

// Files that arrived (Android makes them visible to the gallery and file apps).
void filesReceived(const std::vector<std::string>& paths);

}  // namespace airlink::platform
