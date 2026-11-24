#pragma once
#include "storage.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class Copier {
public:
    // name identifies the destination in log messages, such as "Samsung USB
    // (uuid 34C0-66F7)". One Copier serves one destination.
    Copier(const std::vector<std::string>& sources, std::shared_ptr<Storage> storage, std::string name);

    // The storage changes when a drive is remounted at a different path, and
    // goes away while the destination is unavailable. The sync thread sets the
    // storage again on each change. Null means unavailable.
    void setStorage(std::shared_ptr<Storage> storage);
    std::shared_ptr<Storage> storage() const;

    void handle(const std::string& path, bool is_delete);

    // Both return false when the storage became unavailable partway through,
    // so the caller knows to run the pass again once the storage is back.
    bool syncAll(std::atomic<bool>& stop);
    bool synchronize(std::atomic<bool>& stop);

    // True once after handle() lost a change because the storage was
    // unavailable. The sync thread then runs syncAll() to catch up.
    bool takeResyncRequest();

private:
    // One configured source directory. name is its final path component, which
    // becomes a directory of the same name at the destination.
    struct Source {
        std::string base;
        std::string name;
    };

    std::vector<Source> sources_;
    std::string name_;

    // Guards storage_ only. The watcher thread reads storage_ through handle()
    // while the sync thread replaces storage_.
    mutable std::mutex storage_mutex_;
    std::shared_ptr<Storage> storage_;

    std::atomic<bool> resync_requested_{false};

    std::string destPath(const std::string& src) const;
    bool synchronizeSource(Storage& storage, const Source& source, std::atomic<bool>& stop);
};
