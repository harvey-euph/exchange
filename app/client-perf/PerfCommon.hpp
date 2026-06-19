#pragma once

#include <vector>
#include <string>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <thread>
#include <chrono>

namespace Exchange {

struct Stats {
    uint64_t count = 0;
    double p50 = 0, p90 = 0, p99 = 0, p999 = 0;
};

inline Stats calc_stats(std::vector<double>& rtts) {
    Stats s;
    s.count = rtts.size();
    if (!rtts.empty()) {
        std::sort(rtts.begin(), rtts.end());
        s.p50 = rtts[std::min(static_cast<size_t>(rtts.size() * 0.50), rtts.size() - 1)];
        s.p90 = rtts[std::min(static_cast<size_t>(rtts.size() * 0.90), rtts.size() - 1)];
        s.p99 = rtts[std::min(static_cast<size_t>(rtts.size() * 0.99), rtts.size() - 1)];
        s.p999 = rtts[std::min(static_cast<size_t>(rtts.size() * 0.999), rtts.size() - 1)];
    }
    return s;
}

inline std::string format_stats_row(const std::string& name, const Stats& s) {
    std::ostringstream oss;
    oss << std::left << std::setw(21) << name << std::right << std::setw(9) << s.count << "    ";
    if (s.count > 0) {
        oss << std::fixed << std::setprecision(2) << std::setw(6) << s.p50 << "/ "
            << std::setw(6) << s.p90 << "/ "
            << std::setw(6) << s.p99 << "/ "
            << std::setw(6) << s.p999;
    } else {
        oss << "   0.00/   0.00/   0.00/   0.00";
    }
    return oss.str();
}

inline void high_precision_delay(double sleep_us) {
    if (sleep_us <= 0.0) return;
    if (sleep_us >= 1000.0) {
        std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>(sleep_us)));
    } else {
        auto start = std::chrono::steady_clock::now();
        double target_ns = sleep_us * 1000.0;
        while (true) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count();
            if (elapsed_ns >= target_ns) break;
            #if defined(__x86_64__) || defined(_M_X64)
                __builtin_ia32_pause();
            #else
                std::this_thread::yield();
            #endif
        }
    }
}

inline std::string get_exec_type_name(uint8_t type) {
    switch (type) {
        case 0: return "NEW";
        case 100: return "MODIFY-S";
        case 101: return "MODIFY-L";
        case 4: return "CANCEL";
        case 8: return "REJECT";
        case 5: return "REPLACED";
        default: return "UNKNOWN";
    }
}

} // namespace Exchange
