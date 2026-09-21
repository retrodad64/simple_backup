#pragma once
#include <string>
#include <vector>

// How the config names a removable device. Exactly one identifier is set.
// The identifiers come from the symlink directories udev maintains, so the
// value is whatever `ls /dev/disk/by-uuid` and friends show.
struct DeviceRef {
    std::string uuid;      // filesystem UUID, /dev/disk/by-uuid
    std::string label;     // filesystem label, /dev/disk/by-label
    std::string partuuid;  // partition UUID, /dev/disk/by-partuuid
    std::string serial;    // hardware serial, /dev/disk/by-id
    std::string name;      // shown in log messages, never matched against

    bool empty() const;

    // "Samsung USB (uuid 34C0-66F7)", for log messages.
    std::string describe() const;
};

struct Config {
    std::vector<std::string> sources;

    // Set when the destination is a plain path. Used exactly as written.
    std::string destination;

    // Set when the destination names a device. The backup then goes to
    // <wherever the device is mounted>/device_path.
    DeviceRef device;
    std::string device_path;
    bool mount_device = true;

    bool usesDevice() const;

    static Config load(const std::string& path);

    // Collapses "." and ".." segments and strips any trailing slash, so a
    // source written as "/home/user/Documents/" compares and prints the same
    // as "/home/user/Documents".
    static std::string normalizePath(const std::string& path);
};
