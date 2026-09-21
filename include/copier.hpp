#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

class Copier {
public:
    Copier(const std::vector<std::string>& sources, const std::string& destination);

    // The destination moves when a drive is remounted at a different path, so
    // it is set again whenever the resolver reports a change.
    void setDestination(const std::string& destination);
    std::string destination() const;

    void handle(const std::string& path, bool is_delete);
    void syncAll(std::atomic<bool>& stop);
    void synchronize(std::atomic<bool>& stop);
    bool destAvailable() const;

private:
    // One configured source directory. name is its final path component, which
    // becomes a directory of the same name at the destination.
    struct Source {
        std::string base;
        std::string name;
    };

    std::vector<Source> sources_;

    // Guards dest_ only. The watcher thread reads it through handle() while
    // the sync thread writes it.
    mutable std::mutex dest_mutex_;
    std::string dest_;

    std::string mirrorRoot(const std::string& destination, const Source& source) const;
    std::string destPath(const std::string& destination, const std::string& src) const;
};
