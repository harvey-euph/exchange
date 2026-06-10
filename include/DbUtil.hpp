#pragma once
#include <pqxx/pqxx>
#include <memory>
#include <string>
#include <cstdlib>
#include <iostream>

namespace Exchange {
namespace DbUtil {

inline std::string getConnectionString() {
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
