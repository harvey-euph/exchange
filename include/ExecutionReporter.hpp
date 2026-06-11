#pragma once

#include <cstdint>
#include "fbs/exchange_generated.h"

namespace Exchange {

// Standalone dequeue consumer — reads OrderResponseT from the response ring
// and prints execution reports to stdout for diagnostics / debugging.
class StdoutExecutionReporter
{
public:
    void processResponse(const OrderResponseT& resp);
};

} // namespace Exchange
