#pragma once
#include "config.hpp"
#include "storage.hpp"

#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace remote_files {
class RemoteFileSystem;
class SftpFileSystem;
}

// A directory on a WebDAV or SFTP server, through the remote_files library.
//
// A WebDAV server sets a file's modified time when the upload finishes.
// upToDate() compares the server's time with the local file's, the same way a
// local copy does. A server clock running behind this machine's makes a file
// look out of date, and the next full sync uploads the file again.
//
// An SFTP upload gets the source's modified time, in whole seconds, and the
// source's mode bits when keep_permissions is set. upToDate() then wants the
// same size and the same second, so the server's clock plays no part.
// matches() compares size and permissions, the same as a local copy.
class RemoteFileStorage : public Storage {
public:
    // sftp is files itself when the server speaks SFTP, and null for WebDAV.
    // base is the directory below the file system root holding the backup,
    // such as "Backup", or empty for the root itself. label prefixes every
    // describe() result, such as "https://nas/dav" or "sftp://backup@nas".
    RemoteFileStorage(std::shared_ptr<remote_files::RemoteFileSystem> files, std::shared_ptr<remote_files::SftpFileSystem> sftp,
                      std::string base, std::string label);

    // The storage for the WebDAV or SFTP server a target names. Makes no
    // connection. Reads the password or passphrase file, and throws
    // StorageError when the file won't open or this build lacks the protocol.
    static std::shared_ptr<RemoteFileStorage> create(const Target& target);

    // Creates the backup directory on the server, which tells whether the
    // server answers and accepts the credentials. An SFTP connection that
    // dropped connects again here. On failure, error receives the reason.
    bool probe(std::string& error);

    // Drops the record of directories known to exist. Called whenever the
    // server comes back, since anything on the server changes during an outage.
    void forgetDirectories();

    std::string describe(const std::string& rel) const override;
    bool writable() const override;
    bool exists(const std::string& rel) override;
    std::vector<Entry> list(const std::string& rel) override;
    bool upToDate(const std::filesystem::path& source, const std::string& rel) override;
    bool matches(const std::filesystem::path& source, const std::string& rel) override;
    void createDirectories(const std::string& rel) override;
    void copyFile(const std::filesystem::path& source, const std::string& rel, bool keep_permissions) override;
    void removeAll(const std::string& rel) override;

private:
    std::shared_ptr<remote_files::RemoteFileSystem> files_;
    std::shared_ptr<remote_files::SftpFileSystem> sftp_;
    std::string base_;
    std::string label_;

    // Directories created or seen during this connection, so a run of uploads
    // into one directory doesn't ask the server about its parents each time.
    std::mutex known_mutex_;
    std::set<std::string> known_directories_;

    std::string remote(const std::string& rel) const;
    void ensureDirectory(const std::string& remote_path);
    void remember(const std::string& remote_path);
    void upload(const std::filesystem::path& source, const std::string& remote_path, bool keep_permissions);
};
