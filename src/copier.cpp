#include "copier.hpp"
#include "logger.hpp"

#include <filesystem>
#include <unistd.h>

namespace fs = std::filesystem;

Copier::Copier(const std::vector<std::string>& sources, const std::string& dest)
    : sources_(sources), dest_(dest) {}

bool Copier::destAvailable() const {
    if (dest_.empty()) return false;
    return access(dest_.c_str(), W_OK) == 0;
}

// Returns the destination path that mirrors the source structure under dest_.
// /src/base/rel/file -> dest_/rel/file
std::string Copier::destPath(const std::string& src) const {
    for (const auto& base : sources_) {
        if (src.size() >= base.size() && src.substr(0, base.size()) == base) {
            std::string rel = src.substr(base.size());
            if (!rel.empty() && rel.front() == '/') rel.erase(0, 1);
            return rel.empty() ? dest_ : dest_ + "/" + rel;
        }
    }
    return {};
}

void Copier::handle(const std::string& path, bool is_delete) {
    if (!destAvailable()) {
        Logger::warn("Destination unavailable, skipping: " + path);
        return;
    }

    std::string dest = destPath(path);
    if (dest.empty()) {
        Logger::warn("No matching source base for: " + path);
        return;
    }

    if (is_delete) {
        std::error_code ec;
        fs::remove_all(dest, ec);
        if (!ec)
            Logger::info("Removed: " + dest);
        else
            Logger::warn("Remove failed for " + dest + ": " + ec.message());
        return;
    }

    // Source may have vanished between the event and now.
    if (!fs::exists(path)) return;

    std::error_code ec;
    if (fs::is_directory(path)) {
        fs::create_directories(dest, ec);
        if (ec) Logger::warn("mkdir " + dest + ": " + ec.message());
        else    Logger::info("Dir created: " + dest);
    } else {
        fs::create_directories(fs::path(dest).parent_path(), ec);
        if (ec) { Logger::warn("mkdir parent for " + dest + ": " + ec.message()); return; }
        fs::copy(path, dest, fs::copy_options::overwrite_existing, ec);
        if (ec) Logger::warn("Copy " + path + " -> " + dest + ": " + ec.message());
        else    Logger::info("Copied: " + path + " -> " + dest);
    }
}

void Copier::syncAll(std::atomic<bool>& stop) {
    if (!destAvailable()) return;

    Logger::info("Sync started.");
    std::error_code ec;
    int copied = 0, skipped = 0, failed = 0;

    for (const auto& base : sources_) {
        if (stop) break;

        fs::recursive_directory_iterator it(base,
            fs::directory_options::skip_permission_denied, ec);
        if (ec) {
            Logger::warn("Sync: cannot iterate " + base + ": " + ec.message());
            continue;
        }

        for (const auto& entry : it) {
            if (stop) break;

            const std::string src_path = entry.path().string();
            const std::string dst_path = destPath(src_path);
            if (dst_path.empty()) continue;

            if (entry.is_directory(ec)) {
                fs::create_directories(dst_path, ec);
                if (ec) {
                    Logger::warn("Sync mkdir " + dst_path + ": " + ec.message());
                    ++failed;
                }
                continue;
            }

            if (!entry.is_regular_file(ec)) continue;

            auto src_mtime = fs::last_write_time(entry.path(), ec);
            if (ec) { ++failed; continue; }

            if (fs::exists(dst_path, ec)) {
                auto dst_mtime = fs::last_write_time(dst_path, ec);
                if (!ec && dst_mtime >= src_mtime) { ++skipped; continue; }
            }

            fs::create_directories(fs::path(dst_path).parent_path(), ec);
            fs::copy(entry.path(), dst_path, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                Logger::warn("Sync copy " + src_path + " -> " + dst_path + ": " + ec.message());
                ++failed;
            } else {
                Logger::debug("Synced: " + src_path);
                ++copied;
            }
        }
    }

    Logger::info("Sync complete: " + std::to_string(copied) + " copied, " +
                 std::to_string(skipped) + " up-to-date, " +
                 std::to_string(failed) + " failed.");
}
