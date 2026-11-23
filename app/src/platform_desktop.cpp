// Linux and Windows.

#include "platform.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace airlink::platform {

std::optional<Config> config() {
    return Config{};  // the core's defaults: per-user data dir, Downloads/AirLink, hostname
}

#if defined(__linux__)

namespace {

bool have(const char* program) {
    std::string cmd = std::string("command -v ") + program + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

std::vector<std::string> runPicker() {
    // KDE's own dialog on KDE, GTK's elsewhere. (The XDG portal's FileChooser would serve
    // sandboxed builds too; this is enough for the desktop build.)
    const char* cmd = nullptr;
    const char* desktop = std::getenv("XDG_CURRENT_DESKTOP");
    bool kde = desktop && std::string(desktop).find("KDE") != std::string::npos;
    if ((kde || !have("zenity")) && have("kdialog"))
        cmd = "kdialog --title 'AirLink: dosya gönder' --getopenfilename --multiple --separate-output \"$HOME\" 2>/dev/null";
    else if (have("zenity"))
        cmd = "zenity --file-selection --multiple --separator='\n' --title='AirLink: dosya gönder' 2>/dev/null";
    if (!cmd) return {};

    std::vector<std::string> paths;
    FILE* pipe = popen(cmd, "r");
    if (!pipe) return paths;
    std::string line;
    char buf[4096];
    while (std::fgets(buf, sizeof buf, pipe)) {
        line += buf;
        if (!line.empty() && line.back() == '\n') {
            line.pop_back();
            if (!line.empty()) paths.push_back(line);
            line.clear();
        }
    }
    if (!line.empty()) paths.push_back(line);
    pclose(pipe);
    return paths;
}

}  // namespace

void pickFiles(std::function<void(std::vector<std::string>)> done) {
    std::thread([done = std::move(done)] { done(runPicker()); }).detach();
}

#else

void pickFiles(std::function<void(std::vector<std::string>)> done) {
    // TODO(windows): IFileOpenDialog. Until then files are sent by drag and drop.
    done({});
}

#endif

void stageUris(const std::vector<std::string>&, std::function<void(std::vector<std::string>)> done) {
    done({});  // desktop drops come as paths
}

void filesReceived(const std::vector<std::string>&) {}

}  // namespace airlink::platform
