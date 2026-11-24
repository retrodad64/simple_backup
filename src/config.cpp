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

bool WebDavRef::empty() const {
    return url.empty();
}

std::string WebDavRef::describe() const {
    return name.empty() ? url : name + " (" + url + ")";
}

bool SftpRef::empty() const {
    return host.empty();
}

std::string SftpRef::describe() const {
    std::string address = username.empty() ? host : username + "@" + host;
    if (port != 22) {
        address += ":" + std::to_string(port);
    }

    return name.empty() ? address : name + " (" + address + ")";
}

bool Target::usesDevice() const {
    return !device.empty();
}

bool Target::usesWebDav() const {
    return !webdav.empty();
}

bool Target::usesSftp() const {
    return !sftp.empty();
}

bool Target::usesServer() const {
    return usesWebDav() || usesSftp();
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

static WebDavRef parseWebDav(const nlohmann::json& node) {
    if (!node.is_object()) {
        throw std::runtime_error("destination.webdav must be an object");
    }

    WebDavRef webdav;
    webdav.url                 = node.value("url", "");
    webdav.username            = node.value("username", "");
    webdav.password_file       = node.value("password_file", "");
    webdav.ca_file             = node.value("ca_file", "");
    webdav.name                = node.value("name", "");
    webdav.allow_insecure_http = node.value("allow_insecure_http", false);
    webdav.connect_timeout     = node.value("connect_timeout", 10);

    if (node.contains("password")) {
        throw std::runtime_error("destination.webdav takes password_file, not password, so the password stays out of "
                                 "the config file");
    }

    while (!webdav.url.empty() && webdav.url.back() == '/') {
        webdav.url.pop_back();
    }

    if (webdav.url.empty()) {
        throw std::runtime_error("destination.webdav needs a url");
    }

    if (webdav.url.compare(0, 8, "https://") != 0) {
        if (webdav.url.compare(0, 7, "http://") != 0) {
            throw std::runtime_error("destination.webdav.url must start with https://");
        }

        if (!webdav.allow_insecure_http) {
            throw std::runtime_error("destination.webdav.url uses http://, which sends the password in clear text; "
                                     "use https://, or set allow_insecure_http for a local test server");
        }
    }

    if (!webdav.username.empty() && webdav.password_file.empty()) {
        throw std::runtime_error("destination.webdav has a username but no password_file");
    }

    if (webdav.connect_timeout <= 0) {
        throw std::runtime_error("destination.webdav.connect_timeout must be a number of seconds above 0");
    }

    return webdav;
}

static SftpRef parseSftp(const nlohmann::json& node) {
    if (!node.is_object()) {
        throw std::runtime_error("destination.sftp must be an object");
    }

    SftpRef sftp;
    sftp.host             = node.value("host", "");
    sftp.port             = node.value("port", 22);
    sftp.username         = node.value("username", "");
    sftp.identity_file    = node.value("identity_file", "");
    sftp.passphrase_file  = node.value("passphrase_file", "");
    sftp.known_hosts_file = node.value("known_hosts_file", "");
    sftp.name             = node.value("name", "");
    sftp.connect_timeout  = node.value("connect_timeout", 10);

    if (node.contains("password") || node.contains("password_file")) {
        throw std::runtime_error("destination.sftp logs in with an SSH key; name the private key in identity_file");
    }

    if (node.contains("passphrase")) {
        throw std::runtime_error("destination.sftp takes passphrase_file, not passphrase, so the passphrase stays out of "
                                 "the config file");
    }

    if (sftp.host.empty()) {
        throw std::runtime_error("destination.sftp needs a host");
    }

    if (sftp.port <= 0 || sftp.port > 65535) {
        throw std::runtime_error("destination.sftp.port must be between 1 and 65535");
    }

    if (sftp.connect_timeout <= 0) {
        throw std::runtime_error("destination.sftp.connect_timeout must be a number of seconds above 0");
    }

    return sftp;
}

// The path below the device's mount point or the server's URL. Relative,
// because the mount point is not known until the drive shows up. An SFTP
// server also takes an absolute path. A relative one there starts at the login
// directory.
static std::string parseDevicePath(const nlohmann::json& destination, bool allow_absolute = false) {
    std::string path = destination.value("path", "");
    if (!allow_absolute && !path.empty() && path.front() == '/') {
        throw std::runtime_error("destination.path must be relative to the device or server, so a leading '/' is wrong");
    }

    if (path.find("..") != std::string::npos) {
        throw std::runtime_error("destination.path cannot contain '..'");
    }

    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }

    if (path == "/" && !allow_absolute) {
        path.clear();
    }

    return path;
}

// One destination: a path string, or an object naming a device or a server.
static Target parseTarget(const nlohmann::json& destination) {
    Target target;
    if (destination.is_string()) {
        target.destination = Config::normalizePath(destination.get<std::string>());
        return target;
    }

    if (!destination.is_object()) {
        throw std::runtime_error("destination must be a path string or an object naming a device, a WebDAV server or an "
                                 "SFTP server");
    }

    const bool has_device = destination.contains("device");
    const bool has_webdav = destination.contains("webdav");
    const bool has_sftp = destination.contains("sftp");
    if (static_cast<int>(has_device) + static_cast<int>(has_webdav) + static_cast<int>(has_sftp) != 1) {
        throw std::runtime_error("destination needs exactly one of device, webdav or sftp. List several destinations "
                                 "to back up to more than one place");
    }

    if (has_device) {
        target.device = parseDevice(destination.at("device"));
        target.device_path = parseDevicePath(destination);
        target.mount_device = destination.value("mount", true);
    } else if (has_webdav) {
        target.webdav = parseWebDav(destination.at("webdav"));
        target.webdav_path = parseDevicePath(destination);
    } else {
        target.sftp = parseSftp(destination.at("sftp"));
        target.sftp_path = parseDevicePath(destination, true);
    }

    return target;
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

    // An empty path string adds no target, so the empty config the package
    // installs loads and main() reports the missing destination.
    const auto& destination = json.at("destination");
    if (!destination.is_array()) {
        Target target = parseTarget(destination);
        if (!target.destination.empty() || target.usesDevice() || target.usesServer()) {
            config.targets.push_back(std::move(target));
        }

        return config;
    }

    for (size_t index = 0; index < destination.size(); ++index) {
        try {
            Target target = parseTarget(destination[index]);
            if (target.destination.empty() && !target.usesDevice() && !target.usesServer()) {
                throw std::runtime_error("destination is an empty path");
            }

            config.targets.push_back(std::move(target));
        } catch (const std::exception& error) {
            throw std::runtime_error("destination " + std::to_string(index + 1) + " of " + std::to_string(destination.size()) +
                                     ": " + error.what());
        }
    }

    return config;
}
