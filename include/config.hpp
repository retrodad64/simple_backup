#pragma once
#include <string>
#include <vector>

struct Config {
    std::vector<std::string> sources;
    std::string destination;

    static Config load(const std::string& path);

    // Collapses "." and ".." segments and strips any trailing slash, so a
    // source written as "/home/user/Documents/" compares and prints the same
    // as "/home/user/Documents".
    static std::string normalizePath(const std::string& path);
};
