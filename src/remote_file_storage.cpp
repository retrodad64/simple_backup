#include "remote_file_storage.hpp"

#include <remote_files/remote_files.h>

#include <sys/stat.h>

#include <chrono>
#include <fstream>

namespace fs = std::filesystem;
using remote_files::ErrorCode;
using remote_files::RemoteFileError;

// HTTP dates carry whole seconds. A file uploaded within a second of its last
// write reads as up to a second older than the source.
static constexpr std::chrono::seconds MTIME_TOLERANCE(2);

// A failure every following request will hit too becomes StorageUnavailable,
// so a sync stops instead of logging one failure per file.
[[noreturn]] static void rethrow(const RemoteFileError& error) {
    switch (error.code()) {
    case ErrorCode::Network:
    case ErrorCode::AuthenticationFailed:
    case ErrorCode::InsufficientStorage:
#if REMOTE_FILES_HAS_SSH
    case ErrorCode::HostKeyRejected:
#endif
        throw StorageUnavailable(error.what());
    default:
        break;
    }

    if (error.http_status() >= 500) {
        throw StorageUnavailable(error.what());
    }

    throw StorageError(error.what());
}

// The local file clock has no conversion to the system clock in C++17, so this
// goes through each clock's present moment.
static std::chrono::system_clock::time_point toSystemTime(fs::file_time_type time) {
    using namespace std::chrono;
    return time_point_cast<system_clock::duration>(time - fs::file_time_type::clock::now() + system_clock::now());
}

// The source's modified time in whole seconds, read straight from stat() so
// the upload and the later comparison get the same second.
static bool modifiedSeconds(const fs::path& source, std::chrono::system_clock::time_point& modified) {
    struct stat status;
    if (stat(source.c_str(), &status) != 0) {
        return false;
    }

    modified = std::chrono::system_clock::from_time_t(status.st_mtime);
    return true;
}

// The first line of a file holding a password or a passphrase.
static std::string readSecret(const std::string& path, const std::string& what) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw StorageError("Cannot read the " + what + " file " + path);
    }

    std::string secret;
    std::getline(file, secret);
    if (!secret.empty() && secret.back() == '\r') {
        secret.pop_back();
    }

    return secret;
}

static std::shared_ptr<RemoteFileStorage> createWebDav(const Target& target) {
#if REMOTE_FILES_HAS_CURL
    remote_files::WebDavOptions options;
    options.base_url = target.webdav.url;
    options.username = target.webdav.username;
    options.allow_insecure_http = target.webdav.allow_insecure_http;
    if (!target.webdav.password_file.empty()) {
        options.password = readSecret(target.webdav.password_file, "WebDAV password");
    }

    remote_files::CurlTransportOptions transport;
    transport.ca_file = target.webdav.ca_file;
    transport.connect_timeout = std::chrono::seconds(target.webdav.connect_timeout);
    transport.user_agent = "simple_backup";

    try {
        std::shared_ptr<remote_files::RemoteFileSystem> files =
            remote_files::make_webdav_file_system(std::move(options), std::move(transport));
        return std::make_shared<RemoteFileStorage>(std::move(files), nullptr, target.webdav_path, target.webdav.url);
    } catch (const RemoteFileError& error) {
        throw StorageError(std::string("WebDAV destination: ") + error.what());
    }
#else
    (void)target;
    throw StorageError("the remote_files library this simple_backup was built against has no WebDAV support");
#endif
}

static std::shared_ptr<RemoteFileStorage> createSftp(const Target& target) {
#if REMOTE_FILES_HAS_SSH
    remote_files::SftpOptions options;
    options.host = target.sftp.host;
    options.port = target.sftp.port;
    options.username = target.sftp.username;
    options.identity_file = target.sftp.identity_file;
    options.known_hosts_file = target.sftp.known_hosts_file;
    options.connect_timeout = std::chrono::seconds(target.sftp.connect_timeout);
    if (!target.sftp.passphrase_file.empty()) {
        options.passphrase = readSecret(target.sftp.passphrase_file, "SSH key passphrase");
    }

    // An absolute path is below the server's "/", and a relative one below the
    // login directory. Either way the whole path counts as the backup
    // directory, so probe() creates every missing part of it.
    std::string base = target.sftp_path;
    const bool absolute = !base.empty() && base.front() == '/';
    options.root = absolute ? "/" : "";
    while (!base.empty() && base.front() == '/') {
        base.erase(0, 1);
    }

    std::string label = "sftp://" + (target.sftp.username.empty() ? std::string() : target.sftp.username + "@") + target.sftp.host;
    if (target.sftp.port != 22) {
        label += ":" + std::to_string(target.sftp.port);
    }

    if (!absolute) {
        label += "/~";
    }

    try {
        auto sftp = std::make_shared<remote_files::SftpFileSystem>(std::move(options));
        return std::make_shared<RemoteFileStorage>(sftp, sftp, base, label);
    } catch (const RemoteFileError& error) {
        throw StorageError(std::string("SFTP destination: ") + error.what());
    }
#else
    (void)target;
    throw StorageError("the remote_files library this simple_backup was built against has no SFTP support. "
                       "Rebuild remote_files with libssh");
#endif
}

RemoteFileStorage::RemoteFileStorage(std::shared_ptr<remote_files::RemoteFileSystem> files,
                                     std::shared_ptr<remote_files::SftpFileSystem> sftp, std::string base, std::string label)
    : files_(std::move(files)), sftp_(std::move(sftp)), base_(std::move(base)), label_(std::move(label)) {}

std::shared_ptr<RemoteFileStorage> RemoteFileStorage::create(const Target& target) {
    return target.usesSftp() ? createSftp(target) : createWebDav(target);
}

std::string RemoteFileStorage::remote(const std::string& rel) const {
    if (base_.empty()) {
        return rel;
    }

    return rel.empty() ? base_ : base_ + "/" + rel;
}

void RemoteFileStorage::remember(const std::string& remote_path) {
    std::lock_guard<std::mutex> lock(known_mutex_);
    std::string path = remote_path;
    while (!path.empty()) {
        known_directories_.insert(path);
        const auto slash = path.rfind('/');
        path = slash == std::string::npos ? std::string() : path.substr(0, slash);
    }
}

void RemoteFileStorage::forgetDirectories() {
    std::lock_guard<std::mutex> lock(known_mutex_);
    known_directories_.clear();
}

void RemoteFileStorage::ensureDirectory(const std::string& remote_path) {
    if (remote_path.empty()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(known_mutex_);
        if (known_directories_.count(remote_path) != 0) {
            return;
        }
    }

    try {
        files_->create_directories(remote_path);
    } catch (const RemoteFileError& error) {
        rethrow(error);
    }

    remember(remote_path);
}

bool RemoteFileStorage::probe(std::string& error) {
    try {
        // With no base directory there is nothing to create, so a stat of the
        // root does the asking.
        if (base_.empty()) {
            files_->stat("");
        } else {
            files_->create_directories(base_);
            remember(base_);
        }

        error.clear();
        return true;
    } catch (const RemoteFileError& failure) {
        error = failure.what();
        return false;
    }
}

std::string RemoteFileStorage::describe(const std::string& rel) const {
    const std::string path = remote(rel);
    return path.empty() ? label_ : label_ + "/" + path;
}

bool RemoteFileStorage::writable() const {
    // Reachability is the sync thread's probe() to check. Asking the server
    // before every change would double the requests.
    return true;
}

bool RemoteFileStorage::exists(const std::string& rel) {
    try {
        return files_->exists(remote(rel));
    } catch (const RemoteFileError& error) {
        rethrow(error);
    }
}

std::vector<Storage::Entry> RemoteFileStorage::list(const std::string& rel) {
    std::vector<Entry> entries;
    try {
        for (const auto& entry : files_->list_directory(remote(rel))) {
            entries.push_back(Entry{entry.name, entry.is_directory()});
        }
    } catch (const RemoteFileError& error) {
        if (error.code() == ErrorCode::NotFound || error.code() == ErrorCode::NotADirectory) {
            return entries;
        }

        rethrow(error);
    }

    return entries;
}

bool RemoteFileStorage::upToDate(const fs::path& source, const std::string& rel) {
    remote_files::RemoteEntry entry;
    try {
        entry = files_->stat(remote(rel));
    } catch (const RemoteFileError& error) {
        if (error.code() == ErrorCode::NotFound) {
            return false;
        }

        rethrow(error);
    }

    std::error_code ec;
    const auto size = fs::file_size(source, ec);
    if (ec || entry.is_directory() || entry.size != size || !entry.modified) {
        return false;
    }

    if (sftp_) {
        std::chrono::system_clock::time_point modified;
        return modifiedSeconds(source, modified) && *entry.modified == modified;
    }

    const auto modified = fs::last_write_time(source, ec);
    if (ec) {
        return false;
    }

    return *entry.modified + MTIME_TOLERANCE >= toSystemTime(modified);
}

bool RemoteFileStorage::matches(const fs::path& source, const std::string& rel) {
    // WebDAV keeps no permissions, so the size and time test is all there is.
    if (!sftp_) {
        return upToDate(source, rel);
    }

#if REMOTE_FILES_HAS_SSH
    const std::string path = remote(rel);
    std::error_code ec;
    const auto size = fs::file_size(source, ec);
    if (ec) {
        return false;
    }

    const auto permissions = static_cast<unsigned>(fs::status(source, ec).permissions()) & 07777;
    if (ec) {
        return false;
    }

    try {
        const remote_files::RemoteEntry entry = files_->stat(path);
        if (entry.is_directory() || entry.size != size) {
            return false;
        }

        return sftp_->permissions(path) == permissions;
    } catch (const RemoteFileError& error) {
        if (error.code() == ErrorCode::NotFound) {
            return false;
        }

        rethrow(error);
    }
#else
    return false;
#endif
}

void RemoteFileStorage::createDirectories(const std::string& rel) {
    ensureDirectory(remote(rel));
}

void RemoteFileStorage::upload(const fs::path& source, const std::string& remote_path, bool keep_permissions) {
    files_->upload_file(source, remote_path);

#if REMOTE_FILES_HAS_SSH
    if (!sftp_) {
        return;
    }

    std::chrono::system_clock::time_point modified;
    if (modifiedSeconds(source, modified)) {
        sftp_->set_modified(remote_path, modified);
    }

    if (keep_permissions) {
        std::error_code ec;
        const auto permissions = fs::status(source, ec).permissions();
        if (!ec) {
            sftp_->set_permissions(remote_path, static_cast<unsigned>(permissions) & 07777);
        }
    }
#else
    (void)keep_permissions;
#endif
}

void RemoteFileStorage::copyFile(const fs::path& source, const std::string& rel, bool keep_permissions) {
    const std::string path = remote(rel);
    const auto slash = path.rfind('/');
    const std::string parent = slash == std::string::npos ? std::string() : path.substr(0, slash);

    ensureDirectory(parent);
    try {
        upload(source, path, keep_permissions);
    } catch (const RemoteFileError& error) {
        if (error.code() != ErrorCode::ParentNotFound) {
            rethrow(error);
        }

        // Something removed the directory after the last check. Create the
        // directory again and retry once.
        forgetDirectories();
        ensureDirectory(parent);
        try {
            upload(source, path, keep_permissions);
        } catch (const RemoteFileError& retry_error) {
            rethrow(retry_error);
        }
    }
}

void RemoteFileStorage::removeAll(const std::string& rel) {
    const std::string path = remote(rel);
    try {
        const auto entry = files_->stat(path);
        if (entry.is_directory()) {
            files_->remove_directory(path, remote_files::RemoveMode::Recursive);
        } else {
            files_->remove_file(path);
        }
    } catch (const RemoteFileError& error) {
        if (error.code() == ErrorCode::NotFound) {
            return;
        }

        rethrow(error);
    }

    std::lock_guard<std::mutex> lock(known_mutex_);
    const std::string prefix = path + "/";
    for (auto it = known_directories_.begin(); it != known_directories_.end();) {
        if (*it == path || it->compare(0, prefix.size(), prefix) == 0) {
            it = known_directories_.erase(it);
        } else {
            ++it;
        }
    }
}
