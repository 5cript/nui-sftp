#include "processor_time.hpp"

#include <sys/resource.h>
#include <sys/time.h>

namespace Benchmark
{
    namespace
    {
        std::chrono::nanoseconds durationOf(timeval const& time)
        {
            return std::chrono::seconds{time.tv_sec} + std::chrono::microseconds{time.tv_usec};
        }

        std::chrono::nanoseconds usageOf(int who)
        {
            rusage usage{};
            if (::getrusage(who, &usage) != 0)
                return {};
            return durationOf(usage.ru_utime) + durationOf(usage.ru_stime);
        }
    }

    std::chrono::nanoseconds processorTime()
    {
        return usageOf(RUSAGE_SELF) + usageOf(RUSAGE_CHILDREN);
    }
}
