#pragma once
#include "storage.hpp"

#include <string>

// A directory on a local or mounted file system.
class LocalStorage : public Storage {
public:
    explicit LocalStorage(const std::string& root);

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
    std::string root_;

    std::filesystem::path path(const std::string& rel) const;
};
