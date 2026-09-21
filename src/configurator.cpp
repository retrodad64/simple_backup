#include "configurator.hpp"
#include "config.hpp"
#include "destination.hpp"
#include "process.hpp"

#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace {

const char* SYSTEM_CONFIG_DIR = "/etc/simple_backup";
const char* CONFIG_FILENAME = "simple_backup.json";

std::string trimmed(const std::string& text) {
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }

    const size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

// Reads one line. False means end of input, which counts as aborting.
bool readLine(const std::string& prompt, std::string& line) {
    std::cout << prompt << std::flush;
    std::string raw;
    if (!std::getline(std::cin, raw)) {
        std::cout << "\n";
        return false;
    }

    line = trimmed(raw);
    return true;
}

bool askYesNo(const std::string& question, bool default_yes) {
    while (true) {
        std::string answer;
        if (!readLine(question + (default_yes ? " [Y/n]: " : " [y/N]: "), answer)) {
            return false;
        }

        if (answer.empty()) {
            return default_yes;
        }

        const char first = static_cast<char>(std::tolower(answer[0]));
        if (first == 'y') {
            return true;
        }

        if (first == 'n') {
            return false;
        }

        std::cout << "    Answer y or n.\n";
    }
}

bool parseIndex(const std::string& text, size_t& value) {
    if (text.empty() || !std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); })) {
        return false;
    }

    value = std::strtoul(text.c_str(), nullptr, 10);
    return true;
}

// Reads a menu choice. Zero means abort, and so does end of input.
size_t askChoice(size_t option_count) {
    while (true) {
        std::string answer;
        if (!readLine("Choice: ", answer)) {
            return 0;
        }

        size_t value = 0;
        if (parseIndex(answer, value) && value <= option_count) {
            return value;
        }

        std::cout << "    Enter a number from 0 to " << option_count << ".\n";
    }
}

std::string humanSize(uint64_t bytes) {
    const char* units[] = {"B", "K", "M", "G", "T", "P"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < sizeof(units) / sizeof(units[0])) {
        value /= 1024.0;
        ++unit;
    }

    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), unit == 0 ? "%.0f%s" : "%.1f%s", value, units[unit]);
    return buffer;
}

// Under sudo, HOME belongs to root. The person typing paths is the invoking
// user, so "~" has to expand to their home.
std::string invokingUserHome() {
    const char* sudo_user = std::getenv("SUDO_USER");
    if (sudo_user != nullptr) {
        const passwd* entry = getpwnam(sudo_user);
        if (entry != nullptr) {
            return entry->pw_dir;
        }
    }

    const char* home = std::getenv("HOME");
    if (home != nullptr) {
        return home;
    }

    const passwd* entry = getpwuid(getuid());
    return entry != nullptr ? entry->pw_dir : "";
}

std::string expandUser(const std::string& path) {
    if (path == "~") {
        return invokingUserHome();
    }

    if (path.rfind("~/", 0) == 0) {
        return invokingUserHome() + path.substr(1);
    }

    return path;
}

void printCandidate(size_t index, const DeviceCandidate& candidate) {
    std::string flags;
    if (candidate.removable) {
        flags = "removable";
    } else if (candidate.system) {
        flags = "system";
    }

    // The node is shown as well as the label, because an unlabelled drive is
    // otherwise indistinguishable from another unlabelled drive.
    std::cout << "  " << std::setw(2) << index << ") "
              << std::left << std::setw(20) << (candidate.label.empty() ? "(no label)" : candidate.label)
              << std::setw(17) << candidate.node
              << std::right << std::setw(8) << humanSize(candidate.size_bytes) << "  "
              << std::left << std::setw(6) << (candidate.fstype.empty() ? "-" : candidate.fstype)
              << std::setw(11) << flags
              << (candidate.mount_point.empty() ? "not mounted" : candidate.mount_point)
              << std::right << "\n";
}

bool chooseDevice(DeviceCandidate& chosen) {
    const std::vector<DeviceCandidate> candidates = Destination::listCandidates();
    if (candidates.empty()) {
        std::cout << "No filesystems with a UUID were found. Plug the drive in and try again.\n";
        return false;
    }

    std::cout << "\nWhich device should hold the backup?\n\n";
    for (size_t i = 0; i < candidates.size(); ++i) {
        printCandidate(i + 1, candidates[i]);
    }

    std::cout << "\n   0) abort\n\n";

    const size_t choice = askChoice(candidates.size());
    if (choice == 0) {
        return false;
    }

    chosen = candidates[choice - 1];
    std::cout << "\nChose " << chosen.node << ", uuid " << chosen.uuid << "\n";

    if (chosen.system) {
        std::cout << "That is a system filesystem, so the backup would sit on the same disk as the original.\n";
        if (!askYesNo("Use it anyway?", false)) {
            return false;
        }
    }

    return true;
}

bool ensureMounted(DeviceCandidate& device) {
    if (!device.mount_point.empty()) {
        return true;
    }

    std::cout << device.node << " is not mounted, and it has to be to pick a directory on it.\n";
    if (!askYesNo("Mount it now?", true)) {
        return false;
    }

    std::string mount_point;
    if (!Destination::mountCandidate(device.node, mount_point)) {
        std::cout << "Could not mount it. Mount it yourself, then run this again.\n";
        return false;
    }

    device.mount_point = mount_point;
    device.fstype = Destination::findFilesystemType(device.node);
    std::cout << "Mounted at " << device.mount_point << "\n";
    return true;
}

std::vector<std::string> listDirectories(const std::string& root) {
    std::vector<std::string> names;
    std::error_code ec;
    fs::directory_iterator entries(root, ec);
    if (ec) {
        return names;
    }

    for (const auto& entry : entries) {
        const std::string name = entry.path().filename().string();

        // Hidden entries are the filesystem's own bookkeeping far more often
        // than somewhere a person wants their backup.
        if (name.empty() || name.front() == '.') {
            continue;
        }

        if (entry.is_directory(ec)) {
            names.push_back(name);
        }
    }

    std::sort(names.begin(), names.end());
    return names;
}

bool validRelativePath(const std::string& path) {
    if (path.empty() || path.front() == '/') {
        return false;
    }

    return path.find("..") == std::string::npos;
}

// Asks for a directory on the device. The answer is relative to the mount
// point, because the mount point itself moves.
bool choosePath(const DeviceCandidate& device, std::string& relative) {
    while (true) {
        const std::vector<std::string> directories = listDirectories(device.mount_point);

        std::cout << "\nWhere on " << (device.label.empty() ? device.node : device.label) << " should the backup go?\n\n";
        for (size_t i = 0; i < directories.size(); ++i) {
            std::cout << "  " << std::setw(2) << i + 1 << ") " << directories[i] << "\n";
        }

        const size_t root_option = directories.size() + 1;
        const size_t create_option = directories.size() + 2;
        std::cout << "  " << std::setw(2) << root_option << ") use the top level of the drive\n";
        std::cout << "  " << std::setw(2) << create_option << ") create a new directory\n";
        std::cout << "\n   0) abort\n\n";

        const size_t choice = askChoice(create_option);
        if (choice == 0) {
            return false;
        }

        if (choice == root_option) {
            relative.clear();
            return true;
        }

        if (choice != create_option) {
            relative = directories[choice - 1];
            return true;
        }

        std::string name;
        if (!readLine("Name for the new directory: ", name)) {
            return false;
        }

        if (!validRelativePath(name)) {
            std::cout << "    Give a name relative to the drive, with no leading slash and no '..'.\n";
            continue;
        }

        std::error_code ec;
        fs::create_directories(device.mount_point + "/" + name, ec);
        if (ec) {
            std::cout << "    Could not create it: " << ec.message() << "\n";
            continue;
        }

        relative = name;
        return true;
    }
}

// Reports why a path cannot be a source, or an empty string when it is fine.
std::string sourceProblem(const std::string& path, const std::vector<std::string>& already) {
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return "there is no such path";
    }

    if (!fs::is_directory(path, ec)) {
        return "that is not a directory";
    }

    if (access(path.c_str(), R_OK | X_OK) != 0) {
        return "that directory cannot be read";
    }

    const std::string name = fs::path(path).filename().string();
    for (const auto& existing : already) {
        if (existing == path) {
            return "that one is already on the list";
        }

        if (path.rfind(existing + "/", 0) == 0) {
            return "that is inside " + existing + ", which is already on the list";
        }

        if (existing.rfind(path + "/", 0) == 0) {
            return existing + " is already on the list and sits inside this one";
        }

        if (fs::path(existing).filename().string() == name) {
            return "its name clashes with " + existing + ", and both would back up to the same place";
        }
    }

    return {};
}

bool collectSources(std::vector<std::string>& sources) {
    std::cout << "\nWhich directories should be backed up?\n"
                 "Enter one path per line. Press Enter on an empty line when you are done.\n\n";

    while (true) {
        std::string entry;
        if (!readLine("  source> ", entry)) {
            return false;
        }

        if (entry.empty()) {
            if (sources.empty()) {
                std::cout << "    At least one source is needed.\n";
                continue;
            }

            return true;
        }

        const std::string path = Config::normalizePath(fs::absolute(expandUser(entry)).string());
        const std::string problem = sourceProblem(path, sources);
        if (!problem.empty()) {
            std::cout << "    " << path << ": " << problem << "\n";
            continue;
        }

        sources.push_back(path);
        std::cout << "    added " << path << "\n";
    }
}

void applyOwnership(const fs::path& target, mode_t mode) {
    chmod(target.c_str(), mode);

    // Written while running under sudo, so hand it to the person who ran it
    // instead of leaving a root owned file in their directory.
    const char* sudo_uid = std::getenv("SUDO_UID");
    const char* sudo_gid = std::getenv("SUDO_GID");
    if (geteuid() != 0 || sudo_uid == nullptr || sudo_gid == nullptr) {
        return;
    }

    const uid_t uid = static_cast<uid_t>(std::strtoul(sudo_uid, nullptr, 10));
    const gid_t gid = static_cast<gid_t>(std::strtoul(sudo_gid, nullptr, 10));
    if (chown(target.c_str(), uid, gid) != 0) {
        std::cout << "  Could not change the owner of " << target.string() << "\n";
    }
}

// A directory means "put the usual filename in here". Anything else is taken
// as the full path of the file to write.
bool askForTarget(fs::path& target) {
    std::string answer;
    if (!readLine("Where should the file be written? ", answer)) {
        return false;
    }

    if (answer.empty()) {
        return false;
    }

    fs::path chosen = fs::absolute(expandUser(answer));
    std::error_code ec;
    if (fs::is_directory(chosen, ec)) {
        chosen /= CONFIG_FILENAME;
    }

    target = chosen;
    return true;
}

bool writeFile(const fs::path& target, const std::string& text) {
    std::ofstream file(target);
    if (!file.is_open()) {
        std::cout << "Cannot write " << target.string() << "\n";
        return false;
    }

    file << text << "\n";
    return file.good();
}

// Returns the file that was written, or an empty path when nothing was.
fs::path writeConfiguration(const std::string& text, bool& wrote_system_config) {
    wrote_system_config = false;

    const bool as_root = geteuid() == 0;
    fs::path target = as_root ? fs::path(SYSTEM_CONFIG_DIR) / CONFIG_FILENAME
                              : fs::current_path() / CONFIG_FILENAME;

    if (as_root) {
        std::error_code ec;
        fs::create_directories(SYSTEM_CONFIG_DIR, ec);
    }

    std::error_code ec;
    bool to_system_location = as_root;
    if (fs::exists(target, ec)) {
        std::cout << "\n" << target.string() << " already exists.\n";
        if (!askYesNo("Overwrite it?", false)) {
            if (!askForTarget(target)) {
                std::cout << "Nothing was written.\n";
                return {};
            }

            to_system_location = false;
        }
    }

    if (!writeFile(target, text)) {
        return {};
    }

    if (to_system_location) {
        chmod(target.c_str(), 0640);
    } else {
        applyOwnership(target, 0644);
    }

    wrote_system_config = to_system_location;
    return target;
}

void offerToStartService() {
    std::cout << "\n";
    if (!askYesNo("Start the simple_backup service now?", true)) {
        std::cout << "  Start it later with: systemctl restart simple_backup\n";
        return;
    }

    // Restart rather than start, so a service that is already running picks
    // up the new config.
    if (Process::run({"systemctl", "restart", "simple_backup"}) != 0) {
        std::cout << "  systemctl could not restart the service. Check: systemctl status simple_backup\n";
        return;
    }

    std::cout << "  Service restarted.\n";
    if (Process::run({"systemctl", "is-enabled", "--quiet", "simple_backup"}) != 0) {
        std::cout << "  It will not start at boot. Enable it with: systemctl enable simple_backup\n";
    }
}

}  // namespace

namespace Configurator {

int run() {
    std::cout << "simple_backup configuration\n";

    DeviceCandidate device;
    if (!chooseDevice(device)) {
        std::cout << "Aborted, nothing was written.\n";
        return 1;
    }

    if (!ensureMounted(device)) {
        std::cout << "Aborted, nothing was written.\n";
        return 1;
    }

    std::string relative;
    if (!choosePath(device, relative)) {
        std::cout << "Aborted, nothing was written.\n";
        return 1;
    }

    std::vector<std::string> sources;
    if (!collectSources(sources)) {
        std::cout << "Aborted, nothing was written.\n";
        return 1;
    }

    nlohmann::ordered_json document;
    document["sources"] = sources;

    nlohmann::ordered_json destination;
    nlohmann::ordered_json identifier;
    identifier["uuid"] = device.uuid;
    if (!device.label.empty()) {
        identifier["name"] = device.label;
    }

    destination["device"] = identifier;
    destination["path"] = relative;
    document["destination"] = destination;

    const std::string text = document.dump(4);
    std::cout << "\nThis is the configuration:\n\n" << text << "\n\n";
    if (!askYesNo("Write it?", true)) {
        std::cout << "Nothing was written.\n";
        return 1;
    }

    bool wrote_system_config = false;
    const fs::path target = writeConfiguration(text, wrote_system_config);
    if (target.empty()) {
        return 1;
    }

    // Reading it back proves the service will accept what was just written.
    try {
        Config::load(target.string());
    } catch (const std::exception& error) {
        std::cout << "Written, but it does not load: " << error.what() << "\n";
        return 1;
    }

    std::cout << "\nWrote " << target.string() << "\n";
    if (!wrote_system_config) {
        std::cout << "Move it to " << SYSTEM_CONFIG_DIR << "/" << CONFIG_FILENAME << " to have the service use it.\n";
        return 0;
    }

    offerToStartService();
    return 0;
}

}
