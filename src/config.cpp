#include "config.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <nlohmann/json.hpp>

bool DeviceRef::empty() const {
    return uuid.empty() && label.empty() && partuuid.empty() && serial.empty();
}

std::string DeviceRef::describe() const {
    std::string kind;
    std::string value;
    if (!uuid.empty()) {
        kind = "uuid";
        value = uuid;
    } else if (!label.empty()) {
        kind = "label";
        value = label;
    } else if (!partuuid.empty()) {
        kind = "partuuid";
        value = partuuid;
    } else if (!serial.empty()) {
        kind = "serial";
        value = serial;
    } else {
        return "no device";
    }

    if (name.empty()) {
        return kind + " " + value;
    }

    return name + " (" + kind + " " + value + ")";
}

bool Config::usesDevice() const {
    return !device.empty();
}

std::string Config::normalizePath(const std::string& path) {
    std::string result = std::filesystem::path(path).lexically_normal().string();
    while (result.size() > 1 && result.back() == '/') {
        result.pop_back();
    }

    return result;
}

static DeviceRef parseDevice(const nlohmann::json& node) {
    if (!node.is_object()) {
        throw std::runtime_error("destination.device must be an object");
    }

    DeviceRef device;
    device.uuid     = node.value("uuid", "");
    device.label    = node.value("label", "");
    device.partuuid = node.value("partuuid", "");
    device.serial   = node.value("serial", "");
    device.name     = node.value("name", "");

    int identifiers = 0;
    for (const auto& value : {device.uuid, device.label, device.partuuid, device.serial}) {
        if (!value.empty()) {
            ++identifiers;
        }
    }

    if (identifiers == 0) {
        throw std::runtime_error("destination.device needs one of uuid, label, partuuid or serial");
    }

    if (identifiers > 1) {
        throw std::runtime_error("destination.device has more than one identifier; keep exactly one");
    }

    return device;
}

// The path below the device's mount point. Relative, because the mount point
// is not known until the drive shows up.
static std::string parseDevicePath(const nlohmann::json& destination) {
    std::string path = destination.value("path", "");
    if (!path.empty() && path.front() == '/') {
        throw std::runtime_error("destination.path must be relative to the device, so it cannot start with '/'");
    }

    if (path.find("..") != std::string::npos) {
        throw std::runtime_error("destination.path cannot contain '..'");
    }

    while (!path.empty() && path.back() == '/') {
        path.pop_back();
    }

    return path;
}

Config Config::load(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open config file: " + path);
    }

    nlohmann::json json;
    try {
        file >> json;
    } catch (const std::exception& error) {
        throw std::runtime_error("Config parse error: " + std::string(error.what()));
    }

    Config config;
    for (const auto& source : json.at("sources").get<std::vector<std::string>>()) {
        config.sources.push_back(normalizePath(source));
    }

    const auto& destination = json.at("destination");
    if (destination.is_string()) {
        config.destination = normalizePath(destination.get<std::string>());
    } else if (destination.is_object()) {
        config.device = parseDevice(destination.at("device"));
        config.device_path = parseDevicePath(destination);
        config.mount_device = destination.value("mount", true);
    } else {
        throw std::runtime_error("destination must be a path string or an object naming a device");
    }

    return config;
}
