#pragma once

#include "workload.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace Benchmark
{
    struct Measurement
    {
        Workload const* workload{nullptr};
        Direction direction{Direction::Download};
        std::string client{};
        std::vector<std::chrono::nanoseconds> durations{};
        /** @brief Why the client could not complete a run, the measurement is void then. */
        std::string failure{};
    };

    struct ReportContext
    {
        std::string date{};
        std::string serverVersion{};
        std::string libsshVersion{};
        std::string serverAddress{};
        unsigned int processors{0};
        int repeats{0};
        /** @brief The client every other one is compared with. */
        std::string baselineClient{};
        /** @brief Remarks such as a baseline that could not run. */
        std::vector<std::string> notes{};
    };

    /**
     * @brief The results as a Markdown document with one table row per workload, direction and client.
     */
    std::string renderReport(ReportContext const& context, std::vector<Measurement> const& measurements);

    /** @brief A size in B, KiB, MiB or GiB. */
    std::string formatBytes(std::uint64_t bytes);
}
