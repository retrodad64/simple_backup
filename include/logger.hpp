#pragma once
#include <string>

class Logger {
public:
    static void init(const std::string& ident, bool use_syslog, bool verbose);
    static void debug(const std::string& msg);
    static void info(const std::string& msg);
    static void warn(const std::string& msg);
    static void error(const std::string& msg);

private:
    static bool use_syslog_;
    static bool verbose_;
};
