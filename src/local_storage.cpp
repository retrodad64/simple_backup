#include "local_storage.hpp"

#include "config.hpp"

#include <chrono>
#include <unistd.h>

namespace fs = std::filesystem;

// A copy takes the current time as its mtime, so a file that has not changed
// since it was last backed up tests as up to date. FAT keeps mtimes on a two
// second boundary, so a copy to a FAT destination lands with a timestamp up to
// two seconds earlier than its source. Without this tolerance every sync to a
// USB stick recopies every file.
static constexpr std::chrono::seconds MTIME_TOLERANCE(2);

LocalStorage::LocalStorage(const std::string& root) : root_(Config::normalizePath(root)) {}

fs::path LocalStorage::path(const std::string& rel) const {
    return rel.empty() ? fs::path(root_) : fs::path(root_) / rel;
}

std::string LocalStorage::describe(const std::string& rel) const {
    return path(rel).string();
}

bool LocalStorage::writable() const {
    return !root_.empty() && access(root_.c_str(), W_OK) == 0;
}

bool LocalStorage::exists(const std::string& rel) {
    std::error_code ec;
    return fs::exists(path(rel), ec);
}

std::vector<Storage::Entry> LocalStorage::list(const std::string& rel) {
    std::vector<Entry> entries;
    std::error_code ec;
    fs::directory_iterator it(path(rel), fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        return entries;
    }

    for (fs::directory_iterator end; !ec && it != end; it.increment(ec)) {
        std::error_code type_ec;
        entries.push_back(Entry{it->path().filename().string(), it->is_directory(type_ec)});
    }

    return entries;
}

bool LocalStorage::upToDate(const fs::path& source, const std::string& rel) {
    const fs::path dst = path(rel);
    std::error_code ec;
    if (!fs::exists(dst, ec)) {
        return false;
    }

    const auto src_size = fs::file_size(source, ec);
    if (ec) {
        return false;
    }

    const auto dst_size = fs::file_size(dst, ec);
    if (ec || src_size != dst_size) {
        return false;
    }

    const auto src_time = fs::last_write_time(source, ec);
    if (ec) {
        return false;
    }

    const auto dst_time = fs::last_write_time(dst, ec);
    if (ec) {
        return false;
    }

    return dst_time + MTIME_TOLERANCE >= src_time;
}

bool LocalStorage::matches(const fs::path& source, const std::string& rel) {
    const fs::path dst = path(rel);
    std::error_code ec;
    if (!fs::exists(dst, ec)) {
        return false;
    }

    const auto src_size = fs::file_size(source, ec);
    if (ec) {
        return false;
    }

    const auto dst_size = fs::file_size(dst, ec);
    if (ec || src_size != dst_size) {
        return false;
    }

    const auto src_perms = fs::status(source, ec).permissions();
    if (ec) {
        return false;
    }

    const auto dst_perms = fs::status(dst, ec).permissions();
    return !ec && src_perms == dst_perms;
}

void LocalStorage::createDirectories(const std::string& rel) {
    std::error_code ec;
    fs::create_directories(path(rel), ec);
    if (ec) {
        throw StorageError("mkdir " + path(rel).string() + ": " + ec.message());
    }
}

void LocalStorage::copyFile(const fs::path& source, const std::string& rel, bool keep_permissions) {
    const fs::path dst = path(rel);
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    if (ec) {
        throw StorageError("mkdir parent for " + dst.string() + ": " + ec.message());
    }

    fs::copy(source, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        throw StorageError("copy " + source.string() + " -> " + dst.string() + ": " + ec.message());
    }

    if (keep_permissions) {
        const auto perms = fs::status(source, ec).permissions();
        if (!ec) {
            fs::permissions(dst, perms, ec);
        }
    }
}

void LocalStorage::removeAll(const std::string& rel) {
    std::error_code ec;
    fs::remove_all(path(rel), ec);
    if (ec) {
        throw StorageError("remove " + path(rel).string() + ": " + ec.message());
    }
}
