#include "watcher.hpp"
#include "logger.hpp"

#include <sys/inotify.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace fs = std::filesystem;

// Events on each watched directory.
// IN_CLOSE_WRITE fires once when a file write is complete — far fewer spurious
// copies than IN_MODIFY which fires on every write(2) call.
static constexpr uint32_t WATCH_MASK =
    IN_CLOSE_WRITE | IN_CREATE | IN_DELETE |
    IN_MOVED_FROM  | IN_MOVED_TO |
    IN_DELETE_SELF | IN_MOVE_SELF;

Watcher::Watcher(const std::vector<std::string>& sources) {
    ifd_ = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (ifd_ < 0)
        throw std::runtime_error(std::string("inotify_init1: ") + strerror(errno));

    for (const auto& src : sources) {
        if (!fs::exists(src))
            Logger::warn("Source directory does not exist (skipping): " + src);
        else
            addWatchRecursive(src);
    }
}

Watcher::~Watcher() {
    close(ifd_);
}

void Watcher::addWatchRecursive(const std::string& path) {
    int wd = inotify_add_watch(ifd_, path.c_str(), WATCH_MASK);
    if (wd < 0) {
        Logger::warn("Cannot watch " + path + ": " + strerror(errno));
        return;
    }
    wd_path_[wd] = path;
    Logger::debug("Watching: " + path);

    std::error_code ec;
    fs::recursive_directory_iterator it(path,
        fs::directory_options::skip_permission_denied, ec);
    if (ec) { Logger::warn("Cannot iterate " + path + ": " + ec.message()); return; }

    for (const auto& entry : it) {
        if (!entry.is_directory()) continue;
        int swd = inotify_add_watch(ifd_, entry.path().c_str(), WATCH_MASK);
        if (swd < 0) {
            Logger::warn("Cannot watch " + entry.path().string() + ": " + strerror(errno));
            continue;
        }
        wd_path_[swd] = entry.path().string();
        Logger::debug("Watching: " + entry.path().string());
    }
}

void Watcher::handleEvent(const inotify_event* ev, EventCallback& cb) {
    auto it = wd_path_.find(ev->wd);
    if (it == wd_path_.end()) return;

    const std::string& dir = it->second;
    std::string name(ev->len > 0 ? ev->name : "");
    std::string full = name.empty() ? dir : dir + "/" + name;
    bool is_dir = (ev->mask & IN_ISDIR) != 0;

    if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) {
        wd_path_.erase(it);
        cb({EventType::DirDeleted, full});
        return;
    }

    if (ev->mask & IN_CREATE) {
        if (is_dir) {
            addWatchRecursive(full);
            cb({EventType::DirCreated, full});
        } else {
            cb({EventType::FileCreated, full});
        }
    }

    if (ev->mask & IN_CLOSE_WRITE) {
        cb({EventType::FileModified, full});
    }

    if (ev->mask & IN_DELETE) {
        cb({is_dir ? EventType::DirDeleted : EventType::FileDeleted, full});
    }

    // Treat moves as delete-old / create-new: simpler, correct for backup semantics.
    if (ev->mask & IN_MOVED_FROM) {
        cb({is_dir ? EventType::DirDeleted : EventType::FileDeleted, full});
    }
    if (ev->mask & IN_MOVED_TO) {
        if (is_dir) {
            addWatchRecursive(full);
            cb({EventType::DirCreated, full});
        } else {
            cb({EventType::FileCreated, full});
        }
    }
}

void Watcher::run(EventCallback cb, int signal_fd) {
    alignas(inotify_event) char buf[65536]; // large buffer to drain many events at once

    while (true) {
        pollfd fds[2] = {
            {ifd_,      POLLIN, 0},
            {signal_fd, POLLIN, 0},
        };

        int ret = poll(fds, 2, -1);
        if (ret < 0) {
            if (errno == EINTR) continue;
            Logger::error(std::string("poll: ") + strerror(errno));
            break;
        }

        if (fds[1].revents & POLLIN) {
            Logger::info("Signal received — shutting down.");
            break;
        }

        if (!(fds[0].revents & POLLIN)) continue;

        // Drain all available events before yielding back to poll().
        while (true) {
            ssize_t len = read(ifd_, buf, sizeof(buf));
            if (len < 0) {
                if (errno == EAGAIN) break; // no more data
                Logger::error(std::string("inotify read: ") + strerror(errno));
                break;
            }
            for (char* p = buf; p < buf + len; ) {
                auto* ev = reinterpret_cast<const inotify_event*>(p);
                handleEvent(ev, cb);
                p += sizeof(inotify_event) + ev->len;
            }
        }
    }
}
