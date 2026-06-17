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
    // Full Isolation Profile (>= 6 cores total: core 0 for OS, cores 1-5 dedicated)
    // Priorities fully separated:
    #define ME_CORE 1 // Priority 1 (Matching Engine)
    #define CM_CORE 2 // Priority 2 (Client Manager)
    #define MD_CORE 3 // Priority 3 (Market Data Server)
    #define OH_CORE 4 // Priority 4 (HTTP Accepter)
    #define PD_CORE 5 // Priority 5 (Public Data HTTP Server)

#elif defined(AFFINITY_PROFILE_COMPACT)
    // Compact Core Profile (>= 4 cores total: core 0 for OS, cores 1-2 isolated, core 3 shared)
    // Co-locate publishers, HTTP, and public-data on core 3, isolating ME and CM:
    #define ME_CORE 1 // Priority 1 (Isolated)
    #define CM_CORE 2 // Priority 2 (Isolated)
    #define MD_CORE 3 // Priority 3 (Shared)
    #define OH_CORE 3 // Priority 4 (Shared)
    #define PD_CORE 3 // Priority 5 (Shared)

#elif defined(AFFINITY_PROFILE_SHARED)
    // Shared Cores Profile (>= 2 cores total: core 0 shared, core 1 dedicated to ME)
    // Isolate ME on core 1, everything else on core 0:
    #define ME_CORE 1 // Priority 1 (Isolated)
    #define CM_CORE 0 // Priority 2 (Shared on Core 0)
    #define MD_CORE 0 // Priority 3 (Shared on Core 0)
    #define OH_CORE 0 // Priority 4 (Shared on Core 0)
    #define PD_CORE 0 // Priority 5 (Shared on Core 0)

#else
    // Default Dynamic Profile: reads from environment variables MAIN_CORE
    #define ME_CORE get_env_core("MAIN_CORE", -1)
    #define CM_CORE get_env_core("MAIN_CORE", -1)
    #define MD_CORE get_env_core("MAIN_CORE", -1)
    #define OH_CORE get_env_core("MAIN_CORE", -1)
    #define PD_CORE get_env_core("MAIN_CORE", -1)

#endif
