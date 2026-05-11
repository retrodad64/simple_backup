#pragma once
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

enum class EventType {
    FileModified,
    FileCreated,
    FileDeleted,
    DirCreated,
    DirDeleted,
};

struct WatchEvent {
    EventType type;
    std::string path;
};

using EventCallback = std::function<void(const WatchEvent&)>;

class Watcher {
public:
    explicit Watcher(const std::vector<std::string>& sources);
    ~Watcher();

    // Blocks until signal_fd fires (SIGTERM/SIGINT via signalfd).
    void run(EventCallback cb, int signal_fd);

private:
    int ifd_;
    std::unordered_map<int, std::string> wd_path_;

    void addWatchRecursive(const std::string& path);
    void handleEvent(const struct inotify_event* ev, EventCallback& cb);
};
