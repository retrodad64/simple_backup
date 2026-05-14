#include "config.hpp"
#include "watcher.hpp"
#include "copier.hpp"
#include "logger.hpp"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>
#include <csignal>
#include <sys/signalfd.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

static constexpr const char* DEFAULT_CONFIG =
    "/etc/simple_backup/simple_backup.json";

static void usage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " [--config <path>] [--foreground] [--verbose] [--synchronize]\n"
              << "  --config <path>  Config file (default: " << DEFAULT_CONFIG << ")\n"
              << "  --foreground     Log to stderr instead of syslog\n"
              << "  --verbose        Enable debug logging\n"
              << "  --synchronize    One-shot sync: make dest/<source-name> match source,\n"
              << "                   then exit. Other dirs at the destination root are ignored.\n";
}

int main(int argc, char* argv[]) {
    std::string config_path = DEFAULT_CONFIG;
    bool use_syslog   = true;
    bool verbose       = false;
    bool do_synchronize = false;

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
        else if (arg == "--help" || arg == "-h") {
            usage(argv[0]); return 0;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            usage(argv[0]); return 1;
        }
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
    if (cfg.destination.empty()) {
        Logger::error("No destination configured in " + config_path);
        return 1;
    }

    if (do_synchronize) {
        std::error_code ec;
        std::filesystem::create_directories(cfg.destination, ec);
        Copier copier(cfg.sources, cfg.destination);
        if (!copier.destAvailable()) {
            Logger::error("Destination not available: " + cfg.destination);
            return 1;
        }
        std::atomic<bool> stop{false};
        copier.synchronize(stop);
        return 0;
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
        Copier  copier(cfg.sources, cfg.destination);

        std::atomic<bool> sync_stop{false};
        std::mutex        sync_mu;
        std::condition_variable sync_cv;

        auto sync_sleep = [&](std::chrono::seconds dur) {
            std::unique_lock<std::mutex> lk(sync_mu);
            sync_cv.wait_for(lk, dur, [&]{ return sync_stop.load(); });
        };

        // Sync thread: performs an initial full sync when the destination first
        // becomes available, then re-syncs any time the destination reappears
        // (e.g. after a drive is remounted).
        std::thread sync_thread([&] {
            bool dest_was_available = false;
            while (!sync_stop) {
                // Attempt to create the destination directory each cycle.
                // This is a no-op when it already exists. When the drive is
                // mounted but the directory is missing it creates it, making
                // destAvailable() return true on the same iteration.
                // When the drive is not mounted the parent path is absent and
                // this fails silently — no directory is created on the local fs.
                {
                    std::error_code ec;
                    std::filesystem::create_directories(cfg.destination, ec);
                }
                bool dest_now = copier.destAvailable();
                if (dest_now && !dest_was_available)
                    copier.syncAll(sync_stop);
                dest_was_available = dest_now;
                sync_sleep(std::chrono::seconds(dest_now ? 30 : 5));
            }
        });

        Logger::info("Started — watching " + std::to_string(cfg.sources.size()) +
                     " source(s), destination: " + cfg.destination);

        watcher.run([&copier](const WatchEvent& ev) {
            bool del = (ev.type == EventType::FileDeleted ||
                        ev.type == EventType::DirDeleted);
            copier.handle(ev.path, del);
        }, sfd);

        sync_stop = true;
        sync_cv.notify_all();
        sync_thread.join();
    } catch (const std::exception& e) {
        Logger::error(std::string("Fatal: ") + e.what());
        exit_code = 1;
    }

    close(sfd);
    Logger::info("Stopped.");
    return exit_code;
}
