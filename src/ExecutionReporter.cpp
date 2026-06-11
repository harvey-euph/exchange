#include "ExecutionReporter.hpp"

#include <cstdio>

namespace Exchange {

void StdoutExecutionReporter::processResponse(const OrderResponseT& resp)
{
    const char* exec_name = EnumNameExecType(resp.exec_type);
    const char* side_name = EnumNameSide(resp.side);

    std::printf("[%s] order_id=%lu client_id=%u exec_id=%lu symbol=%u %s price=%ld qty=%lu",
                exec_name, resp.order_id, resp.client_id, resp.exec_id,
                resp.symbol_id, side_name, resp.p, resp.q);

    if (resp.reject_code != RejectCode_None) {
        std::printf(" reject=%s(%d)", EnumNameRejectCode(resp.reject_code),
                    static_cast<int>(resp.reject_code));
    }

    if (resp.engine_latency > 0) {
        std::printf(" engine_lat=%lu", resp.engine_latency);
    }

    std::printf("\n");
}

} // namespace Exchange
