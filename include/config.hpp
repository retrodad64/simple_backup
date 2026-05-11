#pragma once
#include <string>
#include <vector>

struct Config {
    std::vector<std::string> sources;
    std::string destination;

    static Config load(const std::string& path);
};
