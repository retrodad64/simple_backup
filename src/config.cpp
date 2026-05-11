#include "config.hpp"
#include <fstream>
#include <stdexcept>
#include <nlohmann/json.hpp>

Config Config::load(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open())
        throw std::runtime_error("Cannot open config file: " + path);

    nlohmann::json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        throw std::runtime_error("Config parse error: " + std::string(e.what()));
    }

    Config cfg;
    cfg.sources = j.at("sources").get<std::vector<std::string>>();
    cfg.destination = j.at("destination").get<std::string>();
    return cfg;
}
