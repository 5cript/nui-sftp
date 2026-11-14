#pragma once

#include <chrono>

namespace Benchmark
{
    /**
     * @brief The user and system time of this process and of the children it waited for, all threads included.
     */
    std::chrono::nanoseconds processorTime();
}
