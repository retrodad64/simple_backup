#pragma once
#include "config.hpp"

#include <cstdint>
#include <string>
#include <vector>

// A filesystem that could hold a backup, as shown by --create-configuration.
struct DeviceCandidate {
    std::string node;         // /dev/sda1
    std::string uuid;
    std::string label;
    std::string fstype;       // only known while the device is mounted
    std::string mount_point;  // empty when it is not mounted
    uint64_t size_bytes = 0;
    bool removable = false;
    bool system = false;      // holds /, /boot or /boot/efi
};

// Works out where the backup should be written right now. A device backed
// destination moves whenever udisks picks a different mount point, so the
// answer is recomputed instead of being read once from the config.
namespace Destination {

// The directory to back up into, or empty when it is not available yet.
// disk_by_root is where the udev symlink directories live, and only a test
// passes anything but the default.
std::string resolve(const Config& config, const std::string& disk_by_root = "/dev/disk");

// What the config asked for, for log messages.
std::string describe(const Config& config);

// The device node for an identifier, such as "/dev/sda1". Empty when the
// device is not attached. disk_by_root is where the udev symlink directories
// live, and only a test passes anything but the default.
std::string findDeviceNode(const DeviceRef& device, const std::string& disk_by_root = "/dev/disk");

// Where a device node is mounted, or empty when it is not mounted.
std::string findMountPoint(const std::string& device_node);

// The filesystem type of a mounted device node, or empty when it is not
// mounted. The kernel only reports this for a live mount.
std::string findFilesystemType(const std::string& device_node);

// Every filesystem with a UUID, removable ones first. disk_by_root is only
// passed by a test.
std::vector<DeviceCandidate> listCandidates(const std::string& disk_by_root = "/dev/disk");

// Mounts a device through udisks2 and reports where it landed.
bool mountCandidate(const std::string& device_node, std::string& mount_point);

// True when path is the root of a mounted filesystem.
bool isMountPoint(const std::string& path);

// True when path lives on the same filesystem as "/". A backup destination
// that answers true is usually a drive that failed to mount.
bool onRootFilesystem(const std::string& path);

}
