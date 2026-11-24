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

// How the config names a WebDAV server. The backup goes to <url>/<path>.
struct WebDavRef {
    std::string url;            // such as "https://nas.home.lan/dav"
    std::string username;       // empty sends no credentials
    std::string password_file;  // first line is the password; required with username
    std::string ca_file;        // PEM certificate to trust, for a self signed server
    std::string name;           // shown in log messages, never sent
    bool allow_insecure_http = false;
    int connect_timeout = 10;   // seconds

    bool empty() const;

    // "NAS (https://nas.home.lan/dav)", for log messages.
    std::string describe() const;
};

// How the config names an SFTP server. The login is by SSH key only.
struct SftpRef {
    std::string host;              // such as "nas.home.lan"
    int port = 22;
    std::string username;          // empty logs in as the user running simple_backup
    std::string identity_file;     // private key; empty tries the SSH agent, then ~/.ssh/id_*
    std::string passphrase_file;   // first line unlocks the key; empty for a key without a passphrase
    std::string known_hosts_file;  // empty uses ~/.ssh/known_hosts of the user running simple_backup
    std::string name;              // shown in log messages, never sent
    int connect_timeout = 10;      // seconds

    bool empty() const;

    // "NAS (backup@nas.home.lan)", for log messages.
    std::string describe() const;
};

// One place the backup goes. Exactly one of destination, device, webdav and
// sftp is set.
struct Target {
    // Set when the destination is a plain path. Used exactly as written.
    std::string destination;

    // Set when the destination names a device. The backup then goes to
    // <wherever the device is mounted>/device_path.
    DeviceRef device;
    std::string device_path;
    bool mount_device = true;

    // Set when the destination is a WebDAV server. The backup then goes to
    // <webdav.url>/webdav_path.
    WebDavRef webdav;
    std::string webdav_path;

    // Set when the destination is an SFTP server. The backup then goes to
    // sftp_path, which is absolute or relative to the login directory.
    SftpRef sftp;
    std::string sftp_path;

    bool usesDevice() const;
    bool usesWebDav() const;
    bool usesSftp() const;

    // True for a WebDAV or SFTP destination.
    bool usesServer() const;
};

struct Config {
    std::vector<std::string> sources;

    // Every destination gets a full copy of every source, and each one comes
    // and goes on its own.
    std::vector<Target> targets;

    static Config load(const std::string& path);

    // Collapses "." and ".." segments and strips any trailing slash, so a
    // source written as "/home/user/Documents/" compares and prints the same
    // as "/home/user/Documents".
    static std::string normalizePath(const std::string& path);
};
