#include "config.hpp"
#include "copier.hpp"
#include "logger.hpp"

#include <filesystem>
#include <functional>

namespace fs = std::filesystem;

// True when src is base itself or a path underneath it. Compares whole path
// components, so a base of /a/Documents does not match /a/Documents2/file.
// rel receives the part of src below base, and is empty when src is base.
static bool relativeTo(const std::string& base, const std::string& src, std::string& rel) {
    if (src.size() < base.size() || src.compare(0, base.size(), base) != 0) {
        return false;
    }

    if (src.size() == base.size()) {
        rel.clear();
        return true;
    }

    if (src[base.size()] != '/') {
        return false;
    }

    rel = src.substr(base.size() + 1);
    return true;
}

Copier::Copier(const std::vector<std::string>& sources, std::shared_ptr<Storage> storage, std::string name)
    : name_(std::move(name)), storage_(std::move(storage)) {
    for (const auto& path : sources) {
        Source source{Config::normalizePath(path), ""};
        source.name = fs::path(source.base).filename().string();
        if (source.name.empty()) {
            Logger::warn("Source has no directory name, skipping: " + path);
            continue;
        }

        for (const auto& other : sources_) {
            if (other.name == source.name) {
                Logger::warn("Sources " + other.base + " and " + source.base +
                             " share the name '" + source.name + "', so both back up to the same "
                             "directory '" + source.name + "' and will overwrite each other.");
            }
        }

        sources_.push_back(source);
    }
}

void Copier::setStorage(std::shared_ptr<Storage> storage) {
    std::lock_guard<std::mutex> lock(storage_mutex_);
    storage_ = std::move(storage);
}

std::shared_ptr<Storage> Copier::storage() const {
    std::lock_guard<std::mutex> lock(storage_mutex_);
    return storage_;
}

bool Copier::takeResyncRequest() {
    return resync_requested_.exchange(false);
}

// The storage path that mirrors a source path. The source directory's own name
// is recreated at the destination:
// /home/user/Documents/rel/file -> Documents/rel/file
std::string Copier::destPath(const std::string& src) const {
    for (const auto& source : sources_) {
        std::string rel;
        if (!relativeTo(source.base, src, rel)) {
            continue;
        }

        return rel.empty() ? source.name : source.name + "/" + rel;
    }

    return {};
}

void Copier::handle(const std::string& path, bool is_delete) {
    const std::shared_ptr<Storage> storage = this->storage();
    if (!storage || !storage->writable()) {
        Logger::warn("Destination " + name_ + " unavailable, skipping: " + path);
        return;
    }

    const std::string dest = destPath(path);
    if (dest.empty()) {
        Logger::warn("No matching source base for: " + path);
        return;
    }

    try {
        if (is_delete) {
            storage->removeAll(dest);
            Logger::info("Removed: " + storage->describe(dest));
            return;
        }

        // Source may have vanished between the event and now.
        std::error_code ec;
        if (!fs::exists(path, ec)) {
            return;
        }

        if (fs::is_directory(path, ec)) {
            storage->createDirectories(dest);
            Logger::info("Dir created: " + storage->describe(dest));
            return;
        }

        if (storage->upToDate(path, dest)) {
            Logger::debug("Already backed up, skipping: " + path);
            return;
        }

        storage->copyFile(path, dest, false);
        Logger::info("Copied: " + path + " -> " + storage->describe(dest));
    } catch (const StorageUnavailable& error) {
        Logger::warn("Destination " + name_ + " unavailable, " + path + " waits for the next sync: " + error.what());
        resync_requested_ = true;
    } catch (const std::exception& error) {
        Logger::warn(error.what());
    }
}

bool Copier::syncAll(std::atomic<bool>& stop) {
    const std::shared_ptr<Storage> storage = this->storage();
    if (!storage || !storage->writable()) {
        return false;
    }

    Logger::info("Sync started: " + storage->describe(""));
    int copied = 0, skipped = 0, failed = 0;

    try {
        for (const auto& source : sources_) {
            if (stop) {
                break;
            }

            std::error_code ec;
            fs::recursive_directory_iterator it(source.base, fs::directory_options::skip_permission_denied, ec);
            if (ec) {
                Logger::warn("Sync: cannot iterate " + source.base + ": " + ec.message());
                continue;
            }

            // Create the mirror directory even when the source is empty.
            try {
                storage->createDirectories(source.name);
            } catch (const StorageUnavailable&) {
                throw;
            } catch (const std::exception& error) {
                Logger::warn(std::string("Sync ") + error.what());
            }

            for (const auto& entry : it) {
                if (stop) {
                    break;
                }

                const std::string src_path = entry.path().string();
                const std::string dst_path = destPath(src_path);
                if (dst_path.empty()) {
                    continue;
                }

                try {
                    if (entry.is_directory(ec)) {
                        storage->createDirectories(dst_path);
                        continue;
                    }

                    if (!entry.is_regular_file(ec)) {
                        continue;
                    }

                    if (storage->upToDate(src_path, dst_path)) {
                        ++skipped;
                        continue;
                    }

                    storage->copyFile(src_path, dst_path, false);
                    Logger::debug("Synced: " + src_path);
                    ++copied;
                } catch (const StorageUnavailable&) {
                    throw;
                } catch (const std::exception& error) {
                    Logger::warn(std::string("Sync ") + error.what());
                    ++failed;
                }
            }
        }
    } catch (const StorageUnavailable& error) {
        Logger::warn("Sync to " + storage->describe("") + " stopped, destination unavailable: " + error.what());
        return false;
    }

    Logger::info("Sync complete for " + storage->describe("") + ": " + std::to_string(copied) + " copied, " +
                 std::to_string(skipped) + " up-to-date, " +
                 std::to_string(failed) + " failed.");
    return true;
}

bool Copier::synchronize(std::atomic<bool>& stop) {
    const std::shared_ptr<Storage> storage = this->storage();
    if (!storage || !storage->writable()) {
        Logger::error("Destination not available: " + (storage ? storage->describe("") : std::string()));
        return false;
    }

    for (const auto& source : sources_) {
        if (stop) {
            break;
        }

        try {
            if (!synchronizeSource(*storage, source, stop)) {
                return false;
            }
        } catch (const StorageUnavailable& error) {
            Logger::error(std::string("Synchronize stopped, destination unavailable: ") + error.what());
            return false;
        }
    }

    return true;
}

// Makes <storage>/<source name> match one source: copies what is missing or
// different, then removes what the source no longer has.
bool Copier::synchronizeSource(Storage& storage, const Source& source, std::atomic<bool>& stop) {
    const fs::path src_root(source.base);
    std::error_code ec;

    if (!fs::exists(src_root, ec)) {
        Logger::warn("Source not found, skipping: " + source.base);
        return true;
    }

    const std::string mirror_root = source.name;
    if (!storage.exists(mirror_root)) {
        Logger::info("Mirror not found, copying source to: " + storage.describe(mirror_root));
    } else {
        Logger::info("Synchronizing: " + source.base + " -> " + storage.describe(mirror_root));
    }

    storage.createDirectories(mirror_root);
    int copied = 0, skipped = 0, removed = 0, failed = 0;

    // Phase 1: copy/update source -> mirror
    fs::recursive_directory_iterator src_it(src_root, fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        Logger::warn("Cannot iterate source: " + source.base + ": " + ec.message());
        return true;
    }

    for (; src_it != fs::recursive_directory_iterator(); ++src_it) {
        if (stop) {
            break;
        }

        const fs::path rel = fs::relative(src_it->path(), src_root, ec);
        if (ec) {
            ++failed;
            continue;
        }

        const std::string dst = mirror_root + "/" + rel.generic_string();

        try {
            if (src_it->is_directory(ec)) {
                storage.createDirectories(dst);
                continue;
            }

            if (!src_it->is_regular_file(ec)) {
                continue;
            }

            if (storage.matches(src_it->path(), dst)) {
                ++skipped;
                continue;
            }

            storage.copyFile(src_it->path(), dst, true);
            Logger::debug("Synced: " + src_it->path().string());
            ++copied;
        } catch (const StorageUnavailable&) {
            throw;
        } catch (const std::exception& error) {
            Logger::warn(error.what());
            ++failed;
        }
    }

    // Phase 2: remove mirror entries missing from the source. A directory
    // missing from the source goes whole, so its contents aren't visited.
    std::vector<std::string> to_remove;
    std::function<void(const std::string&)> visit = [&](const std::string& rel) {
        for (const auto& entry : storage.list(rel.empty() ? mirror_root : mirror_root + "/" + rel)) {
            if (stop) {
                return;
            }

            const std::string child = rel.empty() ? entry.name : rel + "/" + entry.name;
            std::error_code exists_ec;
            if (!fs::exists(src_root / child, exists_ec)) {
                to_remove.push_back(mirror_root + "/" + child);
            } else if (entry.directory) {
                visit(child);
            }
        }
    };
    visit("");

    for (const auto& path : to_remove) {
        if (stop) {
            break;
        }

        try {
            storage.removeAll(path);
            Logger::debug("Removed: " + storage.describe(path));
            ++removed;
        } catch (const StorageUnavailable&) {
            throw;
        } catch (const std::exception& error) {
            Logger::warn(error.what());
            ++failed;
        }
    }

    Logger::info("Synchronize complete: " + std::to_string(copied) + " copied, " +
                 std::to_string(skipped) + " up-to-date, " +
                 std::to_string(removed) + " removed, " +
                 std::to_string(failed) + " failed.");
    return true;
}
