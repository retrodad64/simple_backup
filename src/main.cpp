#include "config.hpp"
#include "configurator.hpp"
#include "destination.hpp"
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
    if (cfg.destination.empty() && !cfg.usesDevice()) {
        Logger::error("No destination configured in " + config_path);
        return 1;
    }

    if (do_synchronize) {
        const std::string destination = Destination::resolve(cfg);
        if (destination.empty()) {
            Logger::error("Destination not available: " + Destination::describe(cfg));
            return 1;
        }

        Copier copier(cfg.sources, destination);
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
        Copier  copier(cfg.sources, "");

        std::atomic<bool> sync_stop{false};
        std::mutex        sync_mu;
        std::condition_variable sync_cv;

        auto sync_sleep = [&](std::chrono::seconds dur) {
            std::unique_lock<std::mutex> lk(sync_mu);
            sync_cv.wait_for(lk, dur, [&]{ return sync_stop.load(); });
        };

        // Sync thread: resolves the destination every cycle and runs a full
        // sync whenever it changes. A device backed destination changes when
        // the drive is plugged in, and again if udisks picks a different mount
        // point than last time.
        std::thread sync_thread([&] {
            std::string current;
            while (!sync_stop) {
                const std::string resolved = Destination::resolve(cfg);
                if (resolved != current) {
                    copier.setDestination(resolved);
                    current = resolved;

                    if (resolved.empty()) {
                        Logger::info("Destination is gone, waiting for " + Destination::describe(cfg));
                    } else {
                        Logger::info("Destination is " + resolved);
                        if (Destination::onRootFilesystem(resolved)) {
                            Logger::warn("Destination " + resolved + " is on the same filesystem as /, so this is "
                                         "backing up to the local disk. Name the drive by uuid to avoid that.");
                        }

                        copier.syncAll(sync_stop);
                    }
                }

                sync_sleep(std::chrono::seconds(current.empty() ? 5 : 30));
            }
        });

        Logger::info("Started, watching " + std::to_string(cfg.sources.size()) +
                     " source(s), destination: " + Destination::describe(cfg));

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
