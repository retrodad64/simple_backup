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
    std::vector<std::string> sources_;
    std::string dest_;

    std::string destPath(const std::string& src) const;
};
