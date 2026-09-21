#include "destination.hpp"
#include "logger.hpp"
#include "process.hpp"

#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace fs = std::filesystem;

namespace {

bool isOctalDigit(char c) {
    return c >= '0' && c <= '7';
}

bool isHexDigit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// /proc/self/mountinfo writes space, tab, newline and backslash as a three
// digit octal escape, so a mount point reads "/media/michaelg/Samsung\040USB1".
std::string unescapeOctal(const std::string& field) {
    std::string result;
    for (size_t i = 0; i < field.size(); ++i) {
        const bool escape = field[i] == '\\' && i + 3 < field.size() &&
                            isOctalDigit(field[i + 1]) && isOctalDigit(field[i + 2]) && isOctalDigit(field[i + 3]);
        if (escape) {
            result.push_back(static_cast<char>(std::stoi(field.substr(i + 1, 3), nullptr, 8)));
            i += 3;
            continue;
        }

        result.push_back(field[i]);
    }

    return result;
}

// udev escapes characters it considers unsafe in the /dev/disk/by-* symlink
// names, using a different scheme to mountinfo. A label of "Samsung USB"
// becomes the filename "Samsung\x20USB".
std::string unescapeUdev(const std::string& name) {
    std::string result;
    for (size_t i = 0; i < name.size(); ++i) {
        const bool escape = name[i] == '\\' && i + 3 < name.size() && name[i + 1] == 'x' &&
                            isHexDigit(name[i + 2]) && isHexDigit(name[i + 3]);
        if (escape) {
            result.push_back(static_cast<char>(std::stoi(name.substr(i + 2, 2), nullptr, 16)));
            i += 3;
            continue;
        }

        result.push_back(name[i]);
    }

    return result;
}

bool equalsIgnoringCase(const std::string& left, const std::string& right) {
    return left.size() == right.size() && strcasecmp(left.c_str(), right.c_str()) == 0;
}

// Looks through one of the /dev/disk/by-* directories for an entry matching
// wanted, and returns the device node it points at. The entries are compared
// after unescaping, so the config holds "Samsung USB" and not "Samsung\x20USB".
// UUIDs are compared without case, because a FAT UUID is usually written in
// capitals but often typed in lower case.
std::string findInDiskDirectory(const std::string& directory, const std::string& wanted) {
    std::error_code ec;
    fs::directory_iterator entries(directory, ec);
    if (ec) {
        return {};
    }

    for (const auto& entry : entries) {
        if (!equalsIgnoringCase(unescapeUdev(entry.path().filename().string()), wanted)) {
            continue;
        }

        const fs::path node = fs::canonical(entry.path(), ec);
        if (ec) {
            return {};
        }

        return node.string();
    }

    return {};
}

std::string deviceNumber(dev_t device) {
    return std::to_string(major(device)) + ":" + std::to_string(minor(device));
}

bool mountDeviceNode(const std::string& device_node, std::string& mount_point) {
    Logger::info("Mounting " + device_node);

    const int status = Process::run({"udisksctl", "mount", "-b", device_node});

    // udisksctl reports the mount point on stdout, but the wording has changed
    // between releases. Reading the mount table back is version independent,
    // and it also covers someone else mounting the drive in the same moment.
    mount_point = Destination::findMountPoint(device_node);
    if (mount_point.empty()) {
        Logger::warn("Could not mount " + device_node + ", udisksctl exited with " + std::to_string(status));
        return false;
    }

    Logger::info("Mounted " + device_node + " at " + mount_point);
    return true;
}

// The plain path form. The final component is created, its parents are not,
// so a destination naming an absent drive stays unavailable instead of being
// recreated on the local disk.
std::string resolvePath(const std::string& destination) {
    if (destination.empty()) {
        return {};
    }

    std::error_code ec;
    if (!fs::is_directory(destination, ec)) {
        const fs::path parent = fs::path(destination).parent_path();
        if (!fs::is_directory(parent, ec)) {
            return {};
        }

        fs::create_directory(destination, ec);
        if (ec) {
            return {};
        }
    }

    if (access(destination.c_str(), W_OK) != 0) {
        return {};
    }

    return destination;
}

// Joins without doubling the separator, because a device mounted at "/" would
// otherwise produce "//backup".
std::string joinPath(const std::string& base, const std::string& relative) {
    if (relative.empty()) {
        return base;
    }

    if (!base.empty() && base.back() == '/') {
        return base + relative;
    }

    return base + "/" + relative;
}

std::string resolveDevice(const Config& config, const std::string& disk_by_root) {
    const std::string node = Destination::findDeviceNode(config.device, disk_by_root);
    if (node.empty()) {
        return {};
    }

    std::string mount_point = Destination::findMountPoint(node);
    if (mount_point.empty() && config.mount_device) {
        mountDeviceNode(node, mount_point);
    }

    if (mount_point.empty()) {
        return {};
    }

    // Everything below here sits on the device itself, so creating the whole
    // path is safe.
    const std::string destination = joinPath(mount_point, config.device_path);

    std::error_code ec;
    fs::create_directories(destination, ec);
    if (ec) {
        Logger::warn("Cannot create " + destination + ": " + ec.message());
        return {};
    }

    if (access(destination.c_str(), W_OK) != 0) {
        Logger::warn("Destination is not writable: " + destination);
        return {};
    }

    return destination;
}

}  // namespace

namespace Destination {

std::string findDeviceNode(const DeviceRef& device, const std::string& disk_by_root) {
    if (!device.uuid.empty()) {
        return findInDiskDirectory(disk_by_root + "/by-uuid", device.uuid);
    }

    if (!device.partuuid.empty()) {
        return findInDiskDirectory(disk_by_root + "/by-partuuid", device.partuuid);
    }

    if (!device.serial.empty()) {
        return findInDiskDirectory(disk_by_root + "/by-id", device.serial);
    }

    if (!device.label.empty()) {
        return findInDiskDirectory(disk_by_root + "/by-label", device.label);
    }

    return {};
}

// Reads the mount table once and returns the mount point and filesystem type
// of the given device node. Both are empty when it is not mounted.
static void findMount(const std::string& device_node, std::string& mount_point, std::string& fstype) {
    mount_point.clear();
    fstype.clear();

    struct stat node_stat;
    if (stat(device_node.c_str(), &node_stat) != 0) {
        return;
    }

    const std::string wanted = deviceNumber(node_stat.st_rdev);

    std::ifstream mountinfo("/proc/self/mountinfo");
    std::string line;
    while (std::getline(mountinfo, line)) {
        std::istringstream fields(line);
        std::string mount_id;
        std::string parent_id;
        std::string major_minor;
        std::string root;
        std::string candidate;
        if (!(fields >> mount_id >> parent_id >> major_minor >> root >> candidate)) {
            continue;
        }

        if (major_minor != wanted) {
            continue;
        }

        // Skip a bind mount of a subdirectory, which would put the backup
        // somewhere inside the filesystem rather than at its root.
        if (root != "/") {
            continue;
        }

        mount_point = unescapeOctal(candidate);

        // The optional fields end at a lone "-", and the filesystem type is
        // the first field after it.
        const size_t separator = line.find(" - ");
        if (separator != std::string::npos) {
            std::istringstream tail(line.substr(separator + 3));
            tail >> fstype;
        }

        return;
    }
}

std::string findMountPoint(const std::string& device_node) {
    std::string mount_point;
    std::string fstype;
    findMount(device_node, mount_point, fstype);
    return mount_point;
}

std::string findFilesystemType(const std::string& device_node) {
    std::string mount_point;
    std::string fstype;
    findMount(device_node, mount_point, fstype);
    return fstype;
}

bool mountCandidate(const std::string& device_node, std::string& mount_point) {
    return mountDeviceNode(device_node, mount_point);
}

// Reads a single value out of sysfs, such as the size in 512 byte sectors.
static std::string readSysfs(const std::string& path) {
    std::ifstream file(path);
    std::string value;
    std::getline(file, value);
    return value;
}

std::vector<DeviceCandidate> listCandidates(const std::string& disk_by_root) {
    // One pass over by-label first, so each device can be named without
    // rescanning the directory per candidate.
    std::map<std::string, std::string> labels;
    std::error_code ec;
    fs::directory_iterator label_entries(disk_by_root + "/by-label", ec);
    if (!ec) {
        for (const auto& entry : label_entries) {
            const fs::path node = fs::canonical(entry.path(), ec);
            if (ec) {
                continue;
            }

            labels[node.string()] = unescapeUdev(entry.path().filename().string());
        }
    }

    std::vector<DeviceCandidate> candidates;
    fs::directory_iterator uuid_entries(disk_by_root + "/by-uuid", ec);
    if (ec) {
        return candidates;
    }

    for (const auto& entry : uuid_entries) {
        const fs::path node = fs::canonical(entry.path(), ec);
        if (ec) {
            continue;
        }

        DeviceCandidate candidate;
        candidate.node = node.string();
        candidate.uuid = unescapeUdev(entry.path().filename().string());

        const auto label = labels.find(candidate.node);
        if (label != labels.end()) {
            candidate.label = label->second;
        }

        findMount(candidate.node, candidate.mount_point, candidate.fstype);

        const std::string name = node.filename().string();
        const std::string sectors = readSysfs("/sys/class/block/" + name + "/size");
        if (!sectors.empty()) {
            candidate.size_bytes = std::strtoull(sectors.c_str(), nullptr, 10) * 512;
        }

        // The removable flag lives on the whole disk, not the partition.
        candidate.removable = readSysfs("/sys/class/block/" + name + "/../removable") == "1";

        candidate.system = candidate.mount_point == "/" ||
                           candidate.mount_point.rfind("/boot", 0) == 0;

        candidates.push_back(candidate);
    }

    // Removable drives first, since that is what a backup destination usually
    // is, then the rest by name.
    std::sort(candidates.begin(), candidates.end(), [](const DeviceCandidate& left, const DeviceCandidate& right) {
        if (left.removable != right.removable) {
            return left.removable;
        }

        if (left.system != right.system) {
            return right.system;
        }

        return left.node < right.node;
    });

    return candidates;
}

bool isMountPoint(const std::string& path) {
    struct stat path_stat;
    if (stat(path.c_str(), &path_stat) != 0) {
        return false;
    }

    struct stat parent_stat;
    if (stat((path + "/..").c_str(), &parent_stat) != 0) {
        return false;
    }

    // A mount point sits on a different filesystem to the directory holding
    // it. The root directory is its own parent, which is the second case.
    return path_stat.st_dev != parent_stat.st_dev || path_stat.st_ino == parent_stat.st_ino;
}

bool onRootFilesystem(const std::string& path) {
    struct stat path_stat;
    struct stat root_stat;
    if (stat(path.c_str(), &path_stat) != 0 || stat("/", &root_stat) != 0) {
        return false;
    }

    return path_stat.st_dev == root_stat.st_dev;
}

std::string describe(const Config& config) {
    if (config.usesDevice()) {
        if (config.device_path.empty()) {
            return config.device.describe();
        }

        return config.device.describe() + ", path " + config.device_path;
    }

    return config.destination;
}

std::string resolve(const Config& config, const std::string& disk_by_root) {
    if (config.usesDevice()) {
        return resolveDevice(config, disk_by_root);
    }

    return resolvePath(config.destination);
}

}  // namespace Destination
