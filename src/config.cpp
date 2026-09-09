#include "config.hpp"
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <nlohmann/json.hpp>

std::string Config::normalizePath(const std::string& path) {
    std::string s = std::filesystem::path(path).lexically_normal().string();
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

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
    for (const auto& src : j.at("sources").get<std::vector<std::string>>())
        cfg.sources.push_back(normalizePath(src));
    cfg.destination = normalizePath(j.at("destination").get<std::string>());
    return cfg;
}
