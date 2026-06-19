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
    // Full Isolation Profile (>= 5 cores total)
    #define ME_CORE 1 // Priority 1 (Matching Engine)
    #define CM_CORE 2 // Priority 2 (Client Manager)
    #define MD_CORE 3 // Priority 3 (Market Data Server)
    #define OH_CORE 4 // Priority 4 (HTTP Accepter)
    #define PD_CORE 4 // Priority 5 (Public Data HTTP Server)

#elif defined(AFFINITY_PROFILE_4CORE)
    // 4 cores total: core 0 for OS & shared, core 1 for ME, core 2 for CM, core 3 for MD
    #define ME_CORE 1
    #define CM_CORE 2
    #define MD_CORE 3
    #define OH_CORE 0
    #define PD_CORE 0

#elif defined(AFFINITY_PROFILE_3CORE)
    // 3 cores total: core 0 for OS & shared, core 1 for ME, core 2 for CM
    #define ME_CORE 1
    #define CM_CORE 2
    #define MD_CORE 0
    #define OH_CORE 0
    #define PD_CORE 0

#elif defined(AFFINITY_PROFILE_SHARED)
    // Shared Cores Profile (< 3 cores)
    #define ME_CORE 0
    #define CM_CORE 0
    #define MD_CORE 0
    #define OH_CORE 0
    #define PD_CORE 0

#else
    // Default Dynamic Profile: reads from environment variables MAIN_CORE
    #define ME_CORE get_env_core("MAIN_CORE", -1)
    #define CM_CORE get_env_core("MAIN_CORE", -1)
    #define MD_CORE get_env_core("MAIN_CORE", -1)
    #define OH_CORE get_env_core("MAIN_CORE", -1)
    #define PD_CORE get_env_core("MAIN_CORE", -1)

#endif
