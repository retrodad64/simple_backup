#pragma once
#include <atomic>
#include <string>
#include <vector>

class Copier {
public:
    Copier(const std::vector<std::string>& sources, const std::string& dest);

    void handle(const std::string& path, bool is_delete);
    void syncAll(std::atomic<bool>& stop);
    void synchronize(std::atomic<bool>& stop);
    bool destAvailable() const;

private:
    // One configured source directory. name is its final path component, which
    // becomes a directory of the same name at the destination root.
    struct Source {
        std::string base;
        std::string name;
    };

    std::vector<Source> sources_;
    std::string dest_;

    std::string mirrorRoot(const Source& src) const;
    std::string destPath(const std::string& src) const;
};
