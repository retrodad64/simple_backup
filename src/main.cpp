#include "config.hpp"
#include "configurator.hpp"
#include "destination.hpp"
#include "watcher.hpp"
#include "copier.hpp"
#include "local_storage.hpp"
#include "logger.hpp"

#if SIMPLE_BACKUP_REMOTE_FILES
#include "remote_file_storage.hpp"
#endif

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <csignal>
#include <sys/signalfd.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

static constexpr const char* DEFAULT_CONFIG =
    "/etc/simple_backup/simple_backup.json";

// Follows where the backup goes. A device backed destination moves whenever
// udisks picks a different mount point, and a WebDAV or SFTP server comes and
// goes, so refresh() asks again each time the sync thread wakes.
class StorageTracker {
public:
    // Throws when the target names a server and this build has no support for
    // its protocol, or when the password or passphrase file won't open.
    explicit StorageTracker(const Target& target) : target_(target) {
        if (target.usesServer()) {
#if SIMPLE_BACKUP_REMOTE_FILES
            server_ = RemoteFileStorage::create(target);
#else
            throw std::runtime_error("this simple_backup was built without remote_files, so it has no WebDAV or SFTP support");
#endif
        }
    }

    // Names the storage available right now, or returns empty when there is
    // none. The name changes whenever the storage does. reason receives why a
    // server is unavailable.
    std::string refresh(std::string& reason) {
        reason.clear();
#if SIMPLE_BACKUP_REMOTE_FILES
        if (server_) {
            return server_->probe(reason) ? server_->describe("") : std::string();
        }
#endif
        return Destination::resolve(target_);
    }

    // The storage for a name refresh() returned.
    std::shared_ptr<Storage> storage(const std::string& name) {
        if (name.empty()) {
            return nullptr;
        }
#if SIMPLE_BACKUP_REMOTE_FILES
        if (server_) {
            server_->forgetDirectories();
            return server_;
        }
#endif
        return std::make_shared<LocalStorage>(name);
    }

    bool local() const { return !target_.usesServer(); }

private:
    const Target& target_;
#if SIMPLE_BACKUP_REMOTE_FILES
    std::shared_ptr<RemoteFileStorage> server_;
#endif
};

// Runs until stop is set: resolves one destination every cycle and runs a full
// sync whenever the destination changes. A device backed destination changes
// when the drive is plugged in, and again if udisks picks a different mount
// point than last time. A WebDAV or SFTP destination changes when the server
// stops or starts answering. Each destination has a thread of its own, so a
// missing drive or a slow server holds up no other destination.
static void runSyncLoop(StorageTracker& tracker, Copier& copier, const Target& target, std::atomic<bool>& stop,
                        const std::function<void(std::chrono::seconds)>& sleep) {
    const std::string name = Destination::describe(target);
    std::string current;
    std::string last_reason;
    while (!stop) {
        std::string reason;
        const std::string resolved = tracker.refresh(reason);
        if (!reason.empty() && reason != last_reason) {
            Logger::warn("Destination " + name + " unavailable: " + reason);
        }
        last_reason = reason;

        // A change handle() lost to an unavailable destination needs a full
        // sync to catch up, even when nothing else changed.
        const bool resync = copier.takeResyncRequest() && !resolved.empty();
        if (resolved != current || resync) {
            if (resolved != current) {
                copier.setStorage(tracker.storage(resolved));
            }
            current = resolved;

            if (resolved.empty()) {
                Logger::info("Destination is gone, waiting for " + name);
            } else {
                Logger::info("Destination is " + resolved);
                if (tracker.local() && Destination::onRootFilesystem(resolved)) {
                    Logger::warn("Destination " + resolved + " is on the same filesystem as /, so this is "
                                 "backing up to the local disk. Name the drive by uuid to avoid that.");
                }

                // A pass the destination cut short runs again on the next
                // cycle, once the destination answers.
                if (!copier.syncAll(stop)) {
                    copier.setStorage(nullptr);
                    current.clear();
                }
            }
        }

        sleep(std::chrono::seconds(current.empty() ? 5 : 30));
    }
}

// --synchronize: every destination in turn. A destination that is missing or
// stops answering fails the run, and the rest still get their pass.
static int synchronizeAll(const Config& config, std::vector<std::unique_ptr<StorageTracker>>& trackers) {
    int exit_code = 0;
    std::atomic<bool> stop{false};
    for (size_t index = 0; index < trackers.size(); ++index) {
        const std::string name = Destination::describe(config.targets[index]);
        std::string reason;
        const std::string destination = trackers[index]->refresh(reason);
        if (destination.empty()) {
            Logger::error("Destination not available: " + name + (reason.empty() ? std::string() : ": " + reason));
            exit_code = 1;
            continue;
        }

        Copier copier(config.sources, trackers[index]->storage(destination), name);
        if (!copier.synchronize(stop)) {
            exit_code = 1;
        }
    }

    return exit_code;
}

static std::string describeTargets(const Config& config) {
    std::string text;
    for (const Target& target : config.targets) {
        text += (text.empty() ? "" : "; ") + Destination::describe(target);
    }

    return text;
}

static void usage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " [--config <path>] [--foreground] [--verbose] [--synchronize]\n"
              << "  --config <path>  Config file (default: " << DEFAULT_CONFIG << ")\n"
              << "  --foreground     Log to stderr instead of syslog\n"
              << "  --verbose        Enable debug logging\n"
              << "  --synchronize    One-shot sync: make dest/<source-name> match source,\n"
              << "                   then exit. Other dirs at the destination root are ignored.\n"
              << "  --create-configuration\n"
              << "                   Build a config file by answering questions, then exit.\n";
}

int main(int argc, char* argv[]) {
    std::string config_path = DEFAULT_CONFIG;
    bool use_syslog   = true;
    bool verbose       = false;
    bool do_synchronize = false;
    bool do_create_configuration = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--config" || arg == "-c") && i + 1 < argc)
            config_path = argv[++i];
        else if (arg == "--foreground" || arg == "-f")
            use_syslog = false;
        else if (arg == "--verbose" || arg == "-v")
            verbose = true;
        else if (arg == "--synchronize" || arg == "-s")
            do_synchronize = true;
        else if (arg == "--create-configuration")
            do_create_configuration = true;
        else if (arg == "--help" || arg == "-h") {
            usage(argv[0]); return 0;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            usage(argv[0]); return 1;
        }
    }

    // The wizard talks to a terminal, so its own output goes to stdout and any
    // logging goes to stderr beside it rather than to syslog.
    if (do_create_configuration) {
        Logger::init("simple_backup", false, verbose);
        return Configurator::run();
    }

    Logger::init("simple_backup", use_syslog, verbose);

    Config cfg;
    try {
        cfg = Config::load(config_path);
    } catch (const std::exception& e) {
        Logger::error(std::string("Config error: ") + e.what());
        return 1;
    }

    if (cfg.sources.empty()) {
        Logger::error("No source directories configured in " + config_path);
        return 1;
    }
    if (cfg.targets.empty()) {
        Logger::error("No destination configured in " + config_path);
        return 1;
    }

    std::vector<std::unique_ptr<StorageTracker>> trackers;
    for (const Target& target : cfg.targets) {
        try {
            trackers.push_back(std::make_unique<StorageTracker>(target));
        } catch (const std::exception& e) {
            Logger::error("Destination error, " + Destination::describe(target) + ": " + e.what());
            return 1;
        }
    }

    if (do_synchronize) {
        return synchronizeAll(cfg, trackers);
    }

    // Block SIGTERM/SIGINT; deliver via signalfd so poll() wakes cleanly.
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, nullptr);

    int sfd = signalfd(-1, &mask, SFD_CLOEXEC);
    if (sfd < 0) {
        Logger::error(std::string("signalfd: ") + strerror(errno));
        return 1;
    }

    int exit_code = 0;
    try {
        Watcher watcher(cfg.sources);

        std::vector<std::unique_ptr<Copier>> copiers;
        for (const Target& target : cfg.targets) {
            copiers.push_back(std::make_unique<Copier>(cfg.sources, nullptr, Destination::describe(target)));
        }

        std::atomic<bool> sync_stop{false};
        std::mutex        sync_mu;
        std::condition_variable sync_cv;

        const std::function<void(std::chrono::seconds)> sync_sleep = [&](std::chrono::seconds dur) {
            std::unique_lock<std::mutex> lk(sync_mu);
            sync_cv.wait_for(lk, dur, [&]{ return sync_stop.load(); });
        };

        std::vector<std::thread> sync_threads;
        for (size_t index = 0; index < copiers.size(); ++index) {
            sync_threads.emplace_back([&, index] {
                runSyncLoop(*trackers[index], *copiers[index], cfg.targets[index], sync_stop, sync_sleep);
            });
        }

        Logger::info("Started, watching " + std::to_string(cfg.sources.size()) + " source(s), destination" +
                     (cfg.targets.size() == 1 ? ": " : "s: ") + describeTargets(cfg));

        // Each copier skips a change on its own while its destination is away.
        watcher.run([&copiers](const WatchEvent& ev) {
            const bool del = (ev.type == EventType::FileDeleted || ev.type == EventType::DirDeleted);
            for (const auto& copier : copiers) {
                copier->handle(ev.path, del);
            }
        }, sfd);

        sync_stop = true;
        sync_cv.notify_all();
        for (auto& thread : sync_threads) {
            thread.join();
        }
    } catch (const std::exception& e) {
        Logger::error(std::string("Fatal: ") + e.what());
        exit_code = 1;
    }

    close(sfd);
    Logger::info("Stopped.");
    return exit_code;
}
