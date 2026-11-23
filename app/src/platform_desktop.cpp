// Linux and Windows.

#include "platform.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#if defined(__linux__)
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

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

// ---- Launches from a file manager: one instance, later launches hand it their files --------

namespace {

std::mutex shareMutex;
std::function<void(std::vector<std::string>)> shareHandler;
std::vector<std::string> sharedEarly;  // before the handler was set

void deliverShared(std::vector<std::string> paths) {
    if (paths.empty()) return;
    std::function<void(std::vector<std::string>)> handler;
    {
        std::lock_guard lock(shareMutex);
        if (!shareHandler) {
            sharedEarly.insert(sharedEarly.end(), paths.begin(), paths.end());
            return;
        }
        handler = shareHandler;
    }
    handler(std::move(paths));
}

std::vector<std::string> filesFromArgs(int argc, char** argv) {
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--send") continue;
        if (arg.starts_with("file://")) arg = arg.substr(7);  // (%U from some file managers)
        std::error_code ec;
        auto absolute = std::filesystem::absolute(arg, ec);
        if (!ec && std::filesystem::exists(absolute, ec)) files.push_back(absolute.string());
    }
    return files;
}

}  // namespace

#if defined(__linux__)

namespace {

std::string socketPath() {
    if (const char* own = std::getenv("AIRLINK_SOCKET"); own && *own) return own;  // tests
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime && *runtime) return std::string(runtime) + "/airlink.sock";
    return "/tmp/airlink-" + std::to_string(getuid()) + ".sock";
}

sockaddr_un address(const std::string& path) {
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::strncpy(a.sun_path, path.c_str(), sizeof a.sun_path - 1);
    return a;
}

// Hands `files` (one per line) to a running instance. False if none runs.
bool forward(const std::vector<std::string>& files) {
    if (socketPath().size() >= sizeof(sockaddr_un{}.sun_path)) return false;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    sockaddr_un a = address(socketPath());
    if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        close(fd);
        return false;
    }
    std::string message;
    for (const auto& f : files) message += f + "\n";
    for (size_t done = 0; done < message.size();) {
        ssize_t n = write(fd, message.data() + done, message.size() - done);
        if (n <= 0) break;
        done += size_t(n);
    }
    close(fd);
    return true;
}

// Serves later launches: each connection brings files, a line each.
void serve() {
    std::string path = socketPath();
    if (path.size() >= sizeof(sockaddr_un{}.sun_path)) return;  // too long for a socket address
    unlink(path.c_str());  // a stale one (forward() found nobody listening)
    int server = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (server < 0) return;
    sockaddr_un a = address(path);
    if (bind(server, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(server, 4) != 0) {
        close(server);
        return;
    }
    chmod(path.c_str(), 0600);
    std::thread([server] {
        while (true) {
            int client = accept4(server, nullptr, nullptr, SOCK_CLOEXEC);
            if (client < 0) continue;
            std::string data;
            char buf[4096];
            ssize_t n;
            while ((n = read(client, buf, sizeof buf)) > 0) data.append(buf, size_t(n));
            close(client);
            std::vector<std::string> files;
            size_t start = 0;
            for (size_t end; (end = data.find('\n', start)) != std::string::npos; start = end + 1)
                if (end > start) files.push_back(data.substr(start, end - start));
            deliverShared(std::move(files));
        }
    }).detach();
}

}  // namespace

void handleLaunch(int argc, char** argv) {
    std::vector<std::string> files = filesFromArgs(argc, argv);
    // Scripted (headless) runs stay apart from a real instance unless a test names a socket.
    if (std::getenv("KOMPOZ_HEADLESS") && !std::getenv("AIRLINK_SOCKET")) return deliverShared(std::move(files));
    // One instance: a second one would run the same device identity twice.
    if (forward(files)) {
        if (files.empty()) std::fprintf(stderr, "AirLink is already running.\n");
        std::exit(0);
    }
    serve();
    deliverShared(std::move(files));
}

#else

void handleLaunch(int argc, char** argv) {
    // TODO(windows): a named pipe for one instance, as Linux's socket.
    deliverShared(filesFromArgs(argc, argv));
}

#endif

void setShareHandler(std::function<void(std::vector<std::string>)> handler) {
    std::vector<std::string> early;
    {
        std::lock_guard lock(shareMutex);
        shareHandler = std::move(handler);
        early.swap(sharedEarly);
    }
    if (!early.empty()) shareHandler(std::move(early));
}

void nodeStarted(void*) {}

void stageUris(const std::vector<std::string>&, std::function<void(std::vector<std::string>)> done) {
    done({});  // desktop drops come as paths
}

void filesReceived(const std::vector<std::string>&) {}

}  // namespace airlink::platform
