#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>

namespace ScanProgressDetail
{
    using ScanProgressFunction = std::function<void(std::uint64_t, std::uint64_t, std::uint64_t)>;

    /**
     * @brief Hands out a progress callback per scan of scans that run one after another, each reporting the running
     *        total of all of them (bytes, index, entries scanned) to @p report.
     */
    inline std::function<ScanProgressFunction()> sequentialScanProgress(ScanProgressFunction report)
    {
        struct Totals
        {
            std::size_t roots{0};
            std::size_t current{0};
            std::array<std::uint64_t, 3> finished{};
            std::array<std::uint64_t, 3> running{};
        };
        auto totals = std::make_shared<Totals>();
        return [totals, report = std::move(report)]() -> ScanProgressFunction
        {
            const auto root = totals->roots++;
            return [totals, report, root](std::uint64_t bytes, std::uint64_t index, std::uint64_t scanned)
            {
                if (root != totals->current)
                {
                    std::ranges::transform(totals->finished, totals->running, totals->finished.begin(), std::plus{});
                    totals->current = root;
                }
                totals->running = {bytes, index, scanned};
                report(totals->finished[0] + bytes, totals->finished[1] + index, totals->finished[2] + scanned);
            };
        };
    }
}
