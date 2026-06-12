#pragma once
#include <pqxx/pqxx>
#include <memory>
#include <string>
#include <cstdlib>
#include <iostream>
#include <fstream>

namespace Exchange {
namespace DbUtil {

inline void loadEnvFile(const std::string& path = ".env") {
    std::ifstream file(path);
    if (!file.is_open()) return;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto pos = line.find('=');
        if (pos != std::string::npos) {
            std::string key = line.substr(0, pos);
            std::string value = line.substr(pos + 1);
            // setenv(name, value, overwrite)
            setenv(key.c_str(), value.c_str(), 0);
        }
    }
}

inline std::string getConnectionString() {
    loadEnvFile();
    const char* env_conn = std::getenv("DATABASE_URL");
    if (env_conn) {
        return env_conn;
    }
    // Fallback to default peer authentication for local exchange db
    return "dbname=exchange";
}

inline std::unique_ptr<pqxx::connection> getDbConnection() {
    try {
        return std::make_unique<pqxx::connection>(getConnectionString());
    } catch (const std::exception& e) {
        std::cerr << "[DbUtil] Database connection failed: " << e.what() << std::endl;
        throw;
    }
}

} // namespace DbUtil
} // namespace Exchange
