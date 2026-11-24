#pragma once
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

// The file system a backup is written to. Copier works through this interface,
// so the same backup logic writes to a local directory or to a remote server.
//
// Every path given to a Storage is relative to the storage root and separated
// by '/', such as "Documents/2026/report.odt". The empty path names the root.
//
// A failed operation throws StorageError. A failure every later operation will
// hit too, such as a server no longer answering, throws StorageUnavailable
// instead, so a pass over many files stops at the first failure.
//
// One instance is shared by the watcher thread and the sync thread, so an
// implementation has to be safe to call from both at once.
class Storage {
public:
    struct Entry {
        std::string name;
        bool directory = false;
    };

    virtual ~Storage() = default;

    // Where rel lives, for log messages, such as "/media/backup/Backup/a.txt"
    // or "https://nas/dav/Backup/a.txt".
    virtual std::string describe(const std::string& rel) const = 0;

    // A cheap check made before each change. False skips the change.
    virtual bool writable() const = 0;

    virtual bool exists(const std::string& rel) = 0;

    // The entries directly inside directory rel. Empty when rel is missing.
    virtual std::vector<Entry> list(const std::string& rel) = 0;

    // True when rel already holds the current contents of source: same size,
    // and written no earlier than source was.
    virtual bool upToDate(const std::filesystem::path& source, const std::string& rel) = 0;

    // The test --synchronize uses: same size, and the same permissions where
    // the storage keeps permissions at all.
    virtual bool matches(const std::filesystem::path& source, const std::string& rel) = 0;

    // Creates directory rel and any missing parents.
    virtual void createDirectories(const std::string& rel) = 0;

    // Copies a local file to rel, replacing what is there and creating any
    // missing parent directories. keep_permissions copies the mode bits too,
    // where the storage keeps them.
    virtual void copyFile(const std::filesystem::path& source, const std::string& rel, bool keep_permissions) = 0;

    // Deletes rel and everything below rel. A missing rel is no error.
    virtual void removeAll(const std::string& rel) = 0;
};

class StorageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class StorageUnavailable : public StorageError {
public:
    using StorageError::StorageError;
};
