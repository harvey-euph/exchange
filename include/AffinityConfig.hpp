#pragma once
#include <cstdlib>
#include <string>

// Helper to get environment variable with fallback
inline int get_env_core(const char* env_name, int fallback_val) {
    if (const char* env_p = std::getenv(env_name)) {
        try {
            return std::stoi(env_p);
        } catch (...) {}
    }
    return fallback_val;
}

#if defined(AFFINITY_PROFILE_ISOLATED)
    // Full Isolation Profile (>= 7 cores total: core 0 for OS, cores 1-6 dedicated)
    // Priorities fully separated:
    #define ME_MAIN_CORE          1 // Priority 1 (Matching Engine)
    #define CM_MAIN_CORE          2 // Priority 2 (Client Manager)
    #define L2_MAIN_CORE          3 // Priority 3 (L2 Publisher)
    #define L3_MAIN_CORE          4 // Priority 3 (L3 Publisher)
    #define HTTP_MAIN_CORE        5 // Priority 4 (HTTP Accepter)
    #define PUBLIC_DATA_MAIN_CORE 6 // Priority 5 (Public Data HTTP Server)
    #define LOGGER_MAIN_CORE      7 // Priority 6 (Logger)

#elif defined(AFFINITY_PROFILE_COMPACT)
    // Compact Core Profile (>= 4 cores total: core 0 for OS, cores 1-2 isolated, core 3 shared)
    // Co-locate publishers, HTTP, and public-data on core 3, isolating ME and CM:
    #define ME_MAIN_CORE          1 // Priority 1 (Isolated)
    #define CM_MAIN_CORE          2 // Priority 2 (Isolated)
    #define L2_MAIN_CORE          3 // Priority 3 (Shared)
    #define L3_MAIN_CORE          3 // Priority 3 (Shared)
    #define HTTP_MAIN_CORE        3 // Priority 4 (Shared)
    #define PUBLIC_DATA_MAIN_CORE 3 // Priority 5 (Shared)
    #define LOGGER_MAIN_CORE      3 // Priority 6 (Shared)

#elif defined(AFFINITY_PROFILE_SHARED)
    // Shared Cores Profile (>= 2 cores total: core 0 shared, core 1 dedicated to ME)
    // Isolate ME on core 1, everything else on core 0:
    #define ME_MAIN_CORE          1 // Priority 1 (Isolated)
    #define CM_MAIN_CORE          0 // Priority 2 (Shared on Core 0)
    #define L2_MAIN_CORE          0 // Priority 3 (Shared on Core 0)
    #define L3_MAIN_CORE          0 // Priority 3 (Shared on Core 0)
    #define HTTP_MAIN_CORE        0 // Priority 4 (Shared on Core 0)
    #define PUBLIC_DATA_MAIN_CORE 0 // Priority 5 (Shared on Core 0)
    #define LOGGER_MAIN_CORE      0 // Priority 6 (Shared on Core 0)

#else
    // Default Dynamic Profile: reads from environment variables MAIN_CORE
    #define ME_MAIN_CORE          get_env_core("MAIN_CORE", -1)
    #define CM_MAIN_CORE          get_env_core("MAIN_CORE", -1)
    #define L2_MAIN_CORE          get_env_core("MAIN_CORE", -1)
    #define L3_MAIN_CORE          get_env_core("MAIN_CORE", -1)
    #define HTTP_MAIN_CORE        get_env_core("MAIN_CORE", -1)
    #define PUBLIC_DATA_MAIN_CORE get_env_core("MAIN_CORE", -1)
    #define LOGGER_MAIN_CORE      get_env_core("MAIN_CORE", -1)

#endif
