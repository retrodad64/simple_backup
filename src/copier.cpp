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

void Copier::synchronize(std::atomic<bool>& stop) {
    if (!destAvailable()) {
        Logger::error("Destination not available: " + dest_);
        return;
    }

    for (const auto& base : sources_) {
        if (stop) break;

        fs::path src_root(base);
        std::error_code ec;

        if (!fs::exists(src_root, ec)) {
            Logger::warn("Source not found, skipping: " + base);
            continue;
        }

        fs::path mirror_root = fs::path(dest_) / src_root.filename();

        if (!fs::exists(mirror_root, ec)) {
            Logger::info("Mirror not found, copying source to: " + mirror_root.string());
            fs::copy(src_root, mirror_root,
                     fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
            if (ec) Logger::warn("Initial copy failed: " + ec.message());
            else    Logger::info("Copied: " + base + " -> " + mirror_root.string());
            continue;
        }

        Logger::info("Synchronizing: " + base + " -> " + mirror_root.string());
        int copied = 0, skipped = 0, removed = 0, failed = 0;

        // Phase 1: copy/update source → mirror
        fs::recursive_directory_iterator src_it(src_root,
            fs::directory_options::skip_permission_denied, ec);
        if (ec) {
            Logger::warn("Cannot iterate source: " + base + ": " + ec.message());
            continue;
        }

        for (; src_it != fs::recursive_directory_iterator(); ++src_it) {
            if (stop) break;

            fs::path rel = fs::relative(src_it->path(), src_root, ec);
            if (ec) { ++failed; continue; }
            fs::path dst = mirror_root / rel;

            if (src_it->is_directory(ec)) {
                fs::create_directories(dst, ec);
                if (ec) {
                    Logger::warn("mkdir " + dst.string() + ": " + ec.message());
                    ++failed;
                }
                continue;
            }

            if (!src_it->is_regular_file(ec)) continue;

            bool needs_copy = !fs::exists(dst, ec);
            if (!needs_copy) {
                auto src_sz = fs::file_size(src_it->path(), ec);
                auto dst_sz = ec ? static_cast<uintmax_t>(-1) : fs::file_size(dst, ec);
                if (ec || src_sz != dst_sz) {
                    needs_copy = true;
                } else {
                    auto src_perms = fs::status(src_it->path(), ec).permissions();
                    auto dst_perms = ec ? fs::perms::none : fs::status(dst, ec).permissions();
                    needs_copy = ec || (src_perms != dst_perms);
                }
            }

            if (!needs_copy) { ++skipped; continue; }

            fs::create_directories(dst.parent_path(), ec);
            fs::copy(src_it->path(), dst, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                Logger::warn("Copy " + src_it->path().string() + " -> " + dst.string() + ": " + ec.message());
                ++failed;
            } else {
                auto perms = fs::status(src_it->path(), ec).permissions();
                if (!ec) fs::permissions(dst, perms, ec);
                Logger::debug("Synced: " + src_it->path().string());
                ++copied;
            }
        }

        // Phase 2: remove mirror entries that no longer exist in source
        std::vector<fs::path> to_remove;
        {
            fs::recursive_directory_iterator mir_it(mirror_root,
                fs::directory_options::skip_permission_denied, ec);
            if (!ec) {
                for (; mir_it != fs::recursive_directory_iterator(); ++mir_it) {
                    if (stop) break;
                    fs::path rel = fs::relative(mir_it->path(), mirror_root, ec);
                    if (ec) continue;
                    if (!fs::exists(src_root / rel)) {
                        to_remove.push_back(mir_it->path());
                        if (mir_it->is_directory(ec))
                            mir_it.disable_recursion_pending();
                    }
                }
            }
        }

        for (const auto& p : to_remove) {
            if (stop) break;
            std::error_code ec2;
            fs::remove_all(p, ec2);
            if (ec2) {
                Logger::warn("Remove failed: " + p.string() + ": " + ec2.message());
                ++failed;
            } else {
                Logger::debug("Removed: " + p.string());
                ++removed;
            }
        }

        Logger::info("Synchronize complete: " + std::to_string(copied) + " copied, " +
                     std::to_string(skipped) + " up-to-date, " +
                     std::to_string(removed) + " removed, " +
                     std::to_string(failed) + " failed.");
    }
}
