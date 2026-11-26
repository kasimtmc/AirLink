// Linux and Windows.

#include "platform.hpp"

#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <appmodel.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#endif

#if defined(__linux__)
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace airlink::platform {

std::optional<Config> config() {
    return Config{};  // the core's defaults: per-user data dir, Downloads/AirLink, hostname
}

namespace {
// Runs a program without a shell, not waited on.
void spawn(std::vector<std::string> argv) {
#if defined(__linux__)
    std::vector<char*> args;
    for (auto& a : argv) args.push_back(a.data());
    args.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawnp(&pid, args[0], nullptr, nullptr, args.data(), environ) != 0) return log(std::string("cannot run ") + argv[0]);
    std::thread([pid] { waitpid(pid, nullptr, 0); }).detach();
#else
    (void)argv;
#endif
}
}  // namespace

void openFile(const std::string& path) {
    if (path.empty()) return;
#if defined(__linux__)
    spawn({"xdg-open", path});
#elif defined(_WIN32)
    std::wstring wide = std::filesystem::path(std::u8string(path.begin(), path.end())).wstring();
    ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
}

void showInFolder(const std::string& path) {
    if (path.empty()) return;
    const std::filesystem::path file(std::u8string(path.begin(), path.end()));
#if defined(__linux__)
    // The file manager selects it (Dolphin, Nautilus: org.freedesktop.FileManager1), else the folder opens.
    std::error_code ec;
    if (std::filesystem::exists(file, ec)) {
        std::string uri = "file://";
        for (unsigned char c : path) {
            if (std::isalnum(c) || std::strchr("/-._~", c)) uri += char(c);
            else { char hex[4]; std::snprintf(hex, sizeof hex, "%%%02X", c); uri += hex; }
        }
        spawn({"gdbus", "call", "--session", "--dest", "org.freedesktop.FileManager1", "--object-path",
               "/org/freedesktop/FileManager1", "--method", "org.freedesktop.FileManager1.ShowItems", "['" + uri + "']", ""});
        return;
    }
    std::u8string parent = file.parent_path().u8string();
    openFolder(std::string(parent.begin(), parent.end()));  // (the file is gone: its folder)
#elif defined(_WIN32)
    std::wstring args = L"/select,\"" + file.wstring() + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
#endif
}

bool shareWithApps(const std::string&) { return false; }

bool fileExists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(std::u8string(path.begin(), path.end())), ec);
}

std::pair<std::string, std::string> takeSentOriginal(const std::string&) { return {}; }

void openFolder(const std::string& path) {
    if (path.empty()) return;
    const auto utf8 = std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size());
    const std::filesystem::path folder(utf8);
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error) return log("cannot open folder: " + error.message());
#if defined(__linux__)
    pid_t pid = 0;
    const char* args[] = {"xdg-open", path.c_str(), nullptr};
    if (posix_spawnp(&pid, "xdg-open", nullptr, nullptr, const_cast<char* const*>(args), environ) != 0) {
        log("cannot open folder: xdg-open failed");
        return;
    }
    std::thread([pid] { waitpid(pid, nullptr, 0); }).detach();
#elif defined(_WIN32)
    std::wstring wide = folder.wstring();
    if (reinterpret_cast<std::intptr_t>(ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
        log("cannot open folder: ShellExecuteW failed");
#endif
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

#elif defined(_WIN32)

namespace {

std::string toUtf8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(size_t(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

}  // namespace

void pickFiles(std::function<void(std::vector<std::string>)> done) {
    // The system's file dialog, on a thread of its own (COM's single-threaded apartment).
    std::thread([done = std::move(done)] {
        std::vector<std::string> paths;
        if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
            IFileOpenDialog* dialog = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
                DWORD options = 0;
                dialog->GetOptions(&options);
                dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM);
                dialog->SetTitle(L"AirLink: dosya gönder");
                IShellItemArray* items = nullptr;
                if (SUCCEEDED(dialog->Show(nullptr)) && SUCCEEDED(dialog->GetResults(&items))) {
                    DWORD count = 0;
                    items->GetCount(&count);
                    for (DWORD i = 0; i < count; ++i) {
                        IShellItem* item = nullptr;
                        if (FAILED(items->GetItemAt(i, &item))) continue;
                        PWSTR path = nullptr;
                        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                            paths.push_back(toUtf8(path));
                            CoTaskMemFree(path);
                        }
                        item->Release();
                    }
                    items->Release();
                }
                dialog->Release();
            }
            CoUninitialize();
        }
        done(std::move(paths));
    }).detach();
}

#else

void pickFiles(std::function<void(std::vector<std::string>)> done) { done({}); }

#endif

// ---- Launches from a file manager: one instance, later launches hand it their files --------

namespace {

std::mutex shareMutex;
std::function<void(std::vector<std::string>)> shareHandler;
std::vector<std::string> sharedEarly;  // before the handler was set

// A panel widget's click (`airlink --panel X Y`), for takePanelRequest.
std::optional<std::pair<int, int>> panelRequest;
bool backgroundLaunch = false;  // `--background`: started at login

bool hasArg(int argc, char** argv, std::string_view arg) {
    for (int i = 1; i < argc; ++i)
        if (std::string_view(argv[i]) == arg) return true;
    return false;
}
constexpr std::string_view kPanelLine = "\x1fpanel ";  // a line no file path starts with

std::optional<std::pair<int, int>> panelArgs(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) != "--panel") continue;
        if (i + 2 >= argc) return std::pair{0, 0};
        return std::pair{std::atoi(argv[i + 1]), std::atoi(argv[i + 2])};
    }
    return std::nullopt;
}

void deliverPanel(std::pair<int, int> at) {
    std::lock_guard lock(shareMutex);
    panelRequest = at;
}

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
    std::vector<std::string> args;
#if defined(_WIN32)
    // argv is in the ANSI code page: names outside it (Turkish ones, say) would be lost.
    (void)argc, (void)argv;
    int n = 0;
    if (LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &n)) {
        for (int i = 1; i < n; ++i) args.push_back(toUtf8(wide[i]));
        LocalFree(wide);
    }
#else
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
#endif
    std::vector<std::string> files;
    for (std::string arg : args) {
        if (arg == "--send") continue;
        if (arg.starts_with("file://")) arg = arg.substr(7);  // (%U from some file managers)
        if (arg.starts_with("--")) continue;
        std::error_code ec;
        auto absolute = std::filesystem::absolute(std::filesystem::path(std::u8string(arg.begin(), arg.end())), ec);
        if (!ec && std::filesystem::exists(absolute, ec)) {
            std::u8string utf8 = absolute.u8string();
            files.emplace_back(utf8.begin(), utf8.end());
        }
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
            for (size_t end; (end = data.find('\n', start)) != std::string::npos; start = end + 1) {
                std::string line = data.substr(start, end - start);
                if (line.starts_with(kPanelLine)) {
                    int x = 0, y = 0;
                    std::sscanf(line.c_str() + kPanelLine.size(), "%d %d", &x, &y);
                    deliverPanel({x, y});
                } else if (!line.empty()) files.push_back(line);
            }
            deliverShared(std::move(files));
        }
    }).detach();
}

}  // namespace

void handleLaunch(int argc, char** argv) {
    backgroundLaunch = hasArg(argc, argv, "--background");
    std::vector<std::string> files = filesFromArgs(argc, argv);
    auto panel = panelArgs(argc, argv);
    // Scripted (headless) runs stay apart from a real instance unless a test names a socket.
    if (std::getenv("KOMPOZ_HEADLESS") && !std::getenv("AIRLINK_SOCKET")) return deliverShared(std::move(files));
    // One instance: a second one would run the same device identity twice.
    std::vector<std::string> lines = files;
    if (panel) lines.push_back(std::string(kPanelLine) + std::to_string(panel->first) + " " + std::to_string(panel->second));
    if (forward(lines)) {
        if (lines.empty()) std::fprintf(stderr, "AirLink is already running.\n");
        std::exit(0);
    }
    serve();
    if (startAtLogin()) setStartAtLogin(true);  // (kept pointing at this binary)
    if (panel) deliverPanel(*panel);
    deliverShared(std::move(files));
}

#elif defined(_WIN32)

namespace {

std::wstring userName() {
    wchar_t name[256];
    DWORD size = 256;
    return GetUserNameW(name, &size) ? std::wstring(name) : L"user";
}

std::wstring pipeName() { return L"\\\\.\\pipe\\airlink-" + userName(); }

// Hands `files` (one per line) to a running instance. False if none listens.
bool forward(const std::vector<std::string>& files) {
    std::wstring name = pipeName();
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int tries = 0; tries < 20; ++tries) {
        pipe = CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_PIPE_BUSY) return false;
        WaitNamedPipeW(name.c_str(), 200);
    }
    if (pipe == INVALID_HANDLE_VALUE) return false;
    std::string message;
    for (const auto& f : files) message += f + "\n";
    DWORD written = 0;
    WriteFile(pipe, message.data(), DWORD(message.size()), &written, nullptr);
    CloseHandle(pipe);
    return true;
}

void serve() {
    std::thread([] {
        std::wstring name = pipeName();
        bool first = true;
        while (true) {
            // Inbound only; the default security lets only this user (and admins) write to it.
            HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_INBOUND | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                                           PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, PIPE_UNLIMITED_INSTANCES, 0,
                                           64 * 1024, 0, nullptr);
            if (pipe == INVALID_HANDLE_VALUE) return;
            first = false;
            if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
                std::string data;
                char buf[4096];
                DWORD n = 0;
                while (ReadFile(pipe, buf, sizeof buf, &n, nullptr) && n > 0) data.append(buf, n);
                std::vector<std::string> files;
                size_t start = 0;
                for (size_t end; (end = data.find('\n', start)) != std::string::npos; start = end + 1)
                    if (end > start) files.push_back(data.substr(start, end - start));
                deliverShared(std::move(files));
            }
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
        }
    }).detach();
}

// ---- Explorer: "AirLink ile gönder" on files and folders, and AirLink in "Send to" ----------

void setRegistryString(HKEY root, const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    HKEY h;
    if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExW(h, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), DWORD((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
}

std::wstring sendToShortcut() {
    PWSTR folder = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SendTo, 0, nullptr, &folder))) path = std::wstring(folder) + L"\\AirLink.lnk";
    CoTaskMemFree(folder);
    return path;
}

// Per user (HKCU, the user's own Send To folder): no administrator needed. Kept up to date with
// where this .exe is, every start.
void registerShell() {
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return;
    std::wstring command = L"\"" + std::wstring(exe) + L"\" --send \"%1\"";
    for (const wchar_t* type : {L"*", L"Directory"}) {
        std::wstring key = std::wstring(L"Software\\Classes\\") + type + L"\\shell\\AirLink";
        setRegistryString(HKEY_CURRENT_USER, key, nullptr, L"AirLink ile gönder");
        setRegistryString(HKEY_CURRENT_USER, key, L"Icon", std::wstring(exe) + L",0");
        setRegistryString(HKEY_CURRENT_USER, key + L"\\command", nullptr, command);
    }
    // Send To gives all the selected files to one launch.
    std::wstring link = sendToShortcut();
    if (link.empty() || FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return;
    IShellLinkW* shell = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell)))) {
        shell->SetPath(exe);
        shell->SetArguments(L"--send");
        shell->SetDescription(L"AirLink ile gönder");
        IPersistFile* file = nullptr;
        if (SUCCEEDED(shell->QueryInterface(IID_PPV_ARGS(&file)))) {
            file->Save(link.c_str(), TRUE);
            file->Release();
        }
        shell->Release();
    }
    CoUninitialize();
}

void unregisterShell() {
    for (const wchar_t* type : {L"*", L"Directory"})
        RegDeleteTreeW(HKEY_CURRENT_USER, (std::wstring(L"Software\\Classes\\") + type + L"\\shell\\AirLink").c_str());
    if (std::wstring link = sendToShortcut(); !link.empty()) DeleteFileW(link.c_str());
}

}  // namespace

void handleLaunch(int argc, char** argv) {
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--unregister") {
            unregisterShell();
            setStartAtLogin(false);
            std::exit(0);
        }
    backgroundLaunch = hasArg(argc, argv, "--background");
    std::vector<std::string> files = filesFromArgs(argc, argv);
    if (std::getenv("KOMPOZ_HEADLESS")) return deliverShared(std::move(files));
    // One instance. Explorer starts one process per selected file at once: the first takes the
    // mutex and serves; the others wait for its pipe and hand it their file.
    CreateMutexW(nullptr, TRUE, (L"Local\\AirLink-" + userName()).c_str());
    bool another = GetLastError() == ERROR_ALREADY_EXISTS;
    if (std::getenv("AIRLINK_DIAG_LOG"))
        std::fprintf(stderr, "airlink: existing instance: %s\n", another ? "yes" : "no");
    for (int tries = 0; another && tries < 30; ++tries) {
        if (forward(files)) std::exit(0);
        Sleep(100);
    }
    if (another) std::exit(0);  // it never answered; don't run the same identity twice
    serve();
    registerShell();
    if (startAtLogin()) setStartAtLogin(true);  // (kept pointing at this .exe)
    deliverShared(std::move(files));
}

#else

void handleLaunch(int argc, char** argv) { deliverShared(filesFromArgs(argc, argv)); }

#endif

bool launchedAtLogin() { return backgroundLaunch; }

#if defined(__linux__)

namespace {
// ~/.config/autostart/dev.airlink.AirLink.desktop (XDG autostart; KDE, GNOME and the others).
std::filesystem::path autostartFile() {
    // (a Flatpak's XDG_CONFIG_HOME is its own, under ~/.var/app: the host's autostart is ~/.config's)
    const char* config = std::getenv("FLATPAK_ID") ? nullptr : std::getenv("XDG_CONFIG_HOME");
    std::filesystem::path dir = config && *config ? std::filesystem::path(config)
                                                  : std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : "") / ".config";
    return dir / "autostart" / "dev.airlink.AirLink.desktop";
}
}  // namespace

bool canStartAtLogin() { return !std::getenv("KOMPOZ_HEADLESS"); }

bool startAtLogin() {
    std::error_code ec;
    return std::filesystem::exists(autostartFile(), ec);
}

void setStartAtLogin(bool on) {
    std::error_code ec;
    auto file = autostartFile();
    if (!on) {
        std::filesystem::remove(file, ec);
        return;
    }
    // Inside a Flatpak this binary's path is the sandbox's: the host starts it through flatpak.
    std::string command;
    if (const char* app = std::getenv("FLATPAK_ID"); app && *app) {
        command = std::string("flatpak run ") + app;
    } else {
        auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (ec) return log("autostart: cannot find this binary");
        command = "\"" + exe.string() + "\"";
    }
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::trunc);
    out << "[Desktop Entry]\n"
           "Type=Application\n"
           "Name=AirLink\n"
           "Comment=Cihazlar arası bağlantı\n"
           "Exec=" << command << " --background\n"
           "Icon=airlink\n"
           "Terminal=false\n"
           "X-GNOME-Autostart-enabled=true\n"
           "X-KDE-autostart-after=panel\n";
}

#elif defined(_WIN32)

namespace {
constexpr const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

// The packaged install starts through its execution alias, which gives it the package's identity
// (a path into WindowsApps would not); the plain .exe by its own path.
std::wstring loginCommand() {
    UINT32 length = 0;
    bool packaged = GetCurrentPackageFullName(&length, nullptr) != APPMODEL_ERROR_NO_PACKAGE;
    if (packaged) {
        PWSTR apps = nullptr;
        std::wstring alias;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &apps)))
            alias = std::wstring(apps) + L"\\Microsoft\\WindowsApps\\airlink-pkg.exe";
        CoTaskMemFree(apps);
        if (!alias.empty()) return L"\"" + alias + L"\" --background";
    }
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return {};
    return L"\"" + std::wstring(exe) + L"\" --background";
}
}  // namespace

bool canStartAtLogin() { return !std::getenv("KOMPOZ_HEADLESS"); }

bool startAtLogin() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"AirLink", RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

void setStartAtLogin(bool on) {
    HKEY h;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS) return;
    if (std::wstring command = on ? loginCommand() : std::wstring(); !command.empty())
        RegSetValueExW(h, L"AirLink", 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()), DWORD((command.size() + 1) * sizeof(wchar_t)));
    else
        RegDeleteValueW(h, L"AirLink");
    RegCloseKey(h);
}

#else

bool canStartAtLogin() { return false; }
bool startAtLogin() { return false; }
void setStartAtLogin(bool) {}

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

#if defined(__linux__)
namespace {
std::string statusPath() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    return runtime && *runtime ? std::string(runtime) + "/airlink.status" : std::string();
}
}  // namespace

void publishStatus(const std::string& status, const std::string& tooltip, int received) {
    static const std::string path = statusPath();
    if (path.empty()) return;
    static bool cleanup = (std::atexit([] { std::remove(statusPath().c_str()); }), true);
    (void)cleanup;
    std::string temp = path + ".tmp";
    if (FILE* f = std::fopen(temp.c_str(), "w")) {
        std::fprintf(f, "%s\n%s\n%d\n", status.c_str(), tooltip.c_str(), received);
        std::fclose(f);
        std::rename(temp.c_str(), path.c_str());
    }
}
#else
void publishStatus(const std::string&, const std::string&, int) {}
#endif

std::optional<std::pair<int, int>> takePanelRequest() {
    std::lock_guard lock(shareMutex);
    return std::exchange(panelRequest, std::nullopt);
}

void nodeStarted(void*) {}

bool canShareSms() { return false; }
bool smsAccessGranted() { return false; }
void requestSmsAccess() {}
void smsRequest(const std::string&, const std::string&, const std::string&) {}

bool canShareCalls() { return false; }
bool callAccessGranted() { return false; }
void requestCallAccess() {}

bool canShareScreen() { return false; }
bool screenControlEnabled() { return false; }
void openScreenControlSettings() {}
bool batteryRestricted() { return false; }
void requestBatteryExemption() {}
bool needsBackgroundAutostartSettings() { return false; }
void openBackgroundAutostartSettings() {}

void stopPhoneScreenForPeer(const std::string&) {}
void setPauseScreenAudioDuringMusic(bool) {}
void setScreenShareAudio(bool) {}
void chooseMediaAudioPlayers() {}
void setPlayerAudioTarget(const std::string&) {}

bool canUseAdvancedMode() { return false; }
bool canFling() { return false; }
bool flingEnabled() { return false; }
void setFlingEnabled(bool) {}
bool flingMediaAccess() { return false; }
void requestFlingMediaAccess() {}
void startAdvancedSetup() {}
void advancedSetupResult(bool, const std::string&) {}
void helperStarted(bool, const std::string&) {}

void log(const std::string& line) { std::fprintf(stderr, "airlink: %s\n", line.c_str()); }

// ---- Phone notifications: the computer only shows them ---------------------------------------

bool canShareNotifications() { return false; }
bool notificationAccessGranted() { return false; }
void openNotificationAccessSettings() {}
void notificationAction(const std::string&, const std::string&, const std::string&) {}

namespace {
std::atomic<bool> appForeground{true};
}  // namespace

void setAppForeground(bool foreground) { appForeground = foreground; }

void notifyReceived(const std::string& from, const std::vector<std::string>& files, const std::string& text, bool secret) {
    if (appForeground) return;
#if defined(__linux__)
    std::string title, body;
    if (!files.empty()) {
        auto name = [](const std::string& p) { auto s = p.find_last_of('/'); return s == std::string::npos ? p : p.substr(s + 1); };
        title = files.size() == 1 ? from + " bir dosya gönderdi" : from + " " + std::to_string(files.size()) + " dosya gönderdi";
        body = name(files[0]);
        if (files.size() > 1) body += " ve " + std::to_string(files.size() - 1) + " dosya daha";
    } else if (secret) {
        title = from + " bir şifre gönderdi";
        body = "Panoya kondu (gizli)";
    } else {
        title = from + " panosunu paylaştı";
        body = text.size() > 240 ? text.substr(0, 240) + "…" : text;
    }
    std::string first = files.empty() ? std::string() : files[0];
    // notify-send waits for the user's choice and prints it: open the file or its folder.
    std::thread([title, body, first] {
        std::vector<std::string> argv = {"notify-send", "--app-name=AirLink", "--icon=airlink", title, body};
        if (!first.empty()) {
            argv.push_back("--action=open=Aç");
            argv.push_back("--action=folder=Klasörde göster");
        }
        std::vector<char*> args;
        for (auto& a : argv) args.push_back(a.data());
        args.push_back(nullptr);
        int out[2];
        if (pipe(out) != 0) return;
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, out[1], 1);
        posix_spawn_file_actions_addclose(&actions, out[0]);
        pid_t pid = 0;
        bool spawned = posix_spawnp(&pid, "notify-send", &actions, nullptr, args.data(), environ) == 0;
        posix_spawn_file_actions_destroy(&actions);
        close(out[1]);
        std::string chosen;
        char buf[64];
        ssize_t n;
        while ((n = read(out[0], buf, sizeof buf)) > 0) chosen.append(buf, size_t(n));
        close(out[0]);
        if (spawned) waitpid(pid, nullptr, 0);
        if (chosen.starts_with("open")) openFile(first);
        else if (chosen.starts_with("folder")) showInFolder(first);
    }).detach();
#else
    (void)from, (void)files, (void)text, (void)secret;  // TODO(windows): a toast
#endif
}

void passwordAnswered() {}  // (notify-send's question stays until it is closed; answering it again does nothing)

bool clipboardMarkedSensitive() {
#if defined(_WIN32)
    static const UINT exclude = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    return exclude && IsClipboardFormatAvailable(exclude);
#else
    return false;  // (Linux: Kompoz's ClipMetadata.isSensitive)
#endif
}

void askSendPassword(std::function<void(bool)> answer) {
#if defined(__linux__)
    std::thread([answer = std::move(answer)] {
        const char* args[] = {"notify-send", "--app-name=AirLink", "--icon=airlink", "--urgency=normal",
                              "--action=send=Gönder", "--action=skip=Gönderme",
                              "Panoda şifre var", "Güvenilir cihazlarınıza gönderilsin mi?", nullptr};
        int out[2];
        if (pipe(out) != 0) return;
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, out[1], 1);
        posix_spawn_file_actions_addclose(&actions, out[0]);
        pid_t pid = 0;
        bool spawned = posix_spawnp(&pid, "notify-send", &actions, nullptr, const_cast<char* const*>(args), environ) == 0;
        posix_spawn_file_actions_destroy(&actions);
        close(out[1]);
        std::string chosen;
        char buf[32];
        ssize_t n;
        while ((n = read(out[0], buf, sizeof buf)) > 0) chosen.append(buf, size_t(n));
        close(out[0]);
        if (spawned) waitpid(pid, nullptr, 0);
        if (chosen.starts_with("send")) answer(true);
        else if (chosen.starts_with("skip")) answer(false);
    }).detach();
#else
    (void)answer;  // the app's own dialog asks
#endif
}

void showDesktopNotification(const std::string& title, const std::string& body) {
#if defined(__linux__)
    // The desktop's own notifications (freedesktop.org), through notify-send: no shell, so the
    // phone's text is passed as it is.
    std::string t = title, b = body;
    std::thread([t, b] {
        pid_t pid = 0;
        const char* args[] = {"notify-send", "--app-name=AirLink", "--icon=airlink", t.c_str(), b.c_str(), nullptr};
        if (posix_spawnp(&pid, "notify-send", nullptr, nullptr, const_cast<char* const*>(args), environ) == 0)
            waitpid(pid, nullptr, 0);
    }).detach();
#else
    (void)title, (void)body;  // TODO(windows): a toast
#endif
}

void stageUris(const std::vector<std::string>&, std::function<void(std::vector<std::string>)> done) {
    done({});  // desktop drops come as paths
}

void filesReceived(const std::vector<std::string>&) {}

}  // namespace airlink::platform
