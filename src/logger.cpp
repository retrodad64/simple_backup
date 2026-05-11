#include "logger.hpp"
#include <cstdio>
#include <syslog.h>

bool Logger::use_syslog_ = false;
bool Logger::verbose_    = false;

void Logger::init(const std::string& ident, bool use_syslog, bool verbose) {
    use_syslog_ = use_syslog;
    verbose_    = verbose;
    if (use_syslog)
        openlog(ident.c_str(), LOG_PID | LOG_NDELAY, LOG_DAEMON);
}

void Logger::debug(const std::string& msg) {
    if (!verbose_) return;
    if (use_syslog_) syslog(LOG_DEBUG,   "%s", msg.c_str());
    else             fprintf(stderr, "[DEBUG] %s\n", msg.c_str());
}

void Logger::info(const std::string& msg) {
    if (use_syslog_) syslog(LOG_INFO,    "%s", msg.c_str());
    else             fprintf(stderr, "[INFO]  %s\n", msg.c_str());
}

void Logger::warn(const std::string& msg) {
    if (use_syslog_) syslog(LOG_WARNING, "%s", msg.c_str());
    else             fprintf(stderr, "[WARN]  %s\n", msg.c_str());
}

void Logger::error(const std::string& msg) {
    if (use_syslog_) syslog(LOG_ERR,     "%s", msg.c_str());
    else             fprintf(stderr, "[ERROR] %s\n", msg.c_str());
}
