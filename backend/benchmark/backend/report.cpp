#include "report.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <optional>

namespace Benchmark
{
    namespace
    {
        using Seconds = std::chrono::duration<double>;

        std::chrono::nanoseconds median(std::vector<std::chrono::nanoseconds> durations)
        {
            std::ranges::sort(durations);
            return durations[durations.size() / 2];
        }

        std::string formatDuration(std::chrono::nanoseconds duration)
        {
            const auto seconds = Seconds{duration}.count();
            if (seconds < 1.0)
                return fmt::format("{:.1f} ms", seconds * 1000.0);
            return fmt::format("{:.2f} s", seconds);
        }

        double mebibytesPerSecond(std::uint64_t bytes, std::chrono::nanoseconds duration)
        {
            return static_cast<double>(bytes) / (1024.0 * 1024.0) / std::max(Seconds{duration}.count(), 1e-9);
        }

        std::optional<std::chrono::nanoseconds> baselineMedian(
            std::vector<Measurement> const& measurements,
            Measurement const& of,
            std::string const& baselineClient
        )
        {
            const auto baseline = std::ranges::find_if(
                measurements,
                [&](Measurement const& candidate)
                {
                    return candidate.workload == of.workload && candidate.direction == of.direction &&
                        candidate.client == baselineClient && candidate.failure.empty() &&
                        !candidate.durations.empty();
                }
            );
            if (baseline == measurements.end())
                return std::nullopt;
            return median(baseline->durations);
        }

        std::string row(
            Measurement const& measurement,
            std::vector<Measurement> const& measurements,
            std::string const& baselineClient
        )
        {
            auto const& workload = *measurement.workload;
            const auto label = fmt::format(
                "{} ({} × {})", workload.name, workload.fileCount, formatBytes(workload.fileSize)
            );
            if (!measurement.failure.empty() || measurement.durations.empty())
            {
                return fmt::format(
                    "| {} | {} | {} | failed: {} | | | | | |\n",
                    label,
                    toString(measurement.direction),
                    measurement.client,
                    measurement.failure
                );
            }

            const auto middle = median(measurement.durations);
            const auto [fastest, slowest] = std::ranges::minmax(measurement.durations);
            const auto seconds = std::max(Seconds{middle}.count(), 1e-9);
            const auto baseline = baselineMedian(measurements, measurement, baselineClient);
            const auto relative = baseline ? fmt::format("{:.0f} %", 100.0 * Seconds{*baseline}.count() / seconds)
                                           : std::string{"-"};
            return fmt::format(
                "| {} | {} | {} | {} | {} | {} | {:.1f} | {:.0f} | {} |\n",
                label,
                toString(measurement.direction),
                measurement.client,
                formatDuration(middle),
                formatDuration(fastest),
                formatDuration(slowest),
                mebibytesPerSecond(workload.totalBytes(), middle),
                static_cast<double>(workload.fileCount) / seconds,
                relative
            );
        }
    }

    std::string formatBytes(std::uint64_t bytes)
    {
        constexpr std::uint64_t kibibyte = 1024u;
        if (bytes >= kibibyte * kibibyte * kibibyte && bytes % (kibibyte * kibibyte * kibibyte) == 0)
            return fmt::format("{} GiB", bytes / (kibibyte * kibibyte * kibibyte));
        if (bytes >= kibibyte * kibibyte && bytes % (kibibyte * kibibyte) == 0)
            return fmt::format("{} MiB", bytes / (kibibyte * kibibyte));
        if (bytes >= kibibyte && bytes % kibibyte == 0)
            return fmt::format("{} KiB", bytes / kibibyte);
        return fmt::format("{} B", bytes);
    }

    std::string renderReport(ReportContext const& context, std::vector<Measurement> const& measurements)
    {
        std::string report = fmt::format(
            "# nui-sftp transfer benchmark\n"
            "\n"
            "- Date: {}\n"
            "- Server: {} in docker at {}, files on a tmpfs\n"
            "- Client library: libssh {}\n"
            "- Processors: {}\n"
            "- Runs per row: {}, the median is shown\n"
            "\n"
            "| Workload | Direction | Client | Median | Fastest | Slowest | MiB/s | Files/s | Speed vs {} |\n"
            "|---|---|---|---:|---:|---:|---:|---:|---:|\n",
            context.date,
            context.serverVersion,
            context.serverAddress,
            context.libsshVersion,
            context.processors,
            context.repeats,
            context.baselineClient
        );
        for (auto const& measurement : measurements)
            report += row(measurement, measurements, context.baselineClient);

        report +=
            "\n"
            "## Clients\n"
            "\n"
            "- **nui-sftp**: the app's operations driven like Session::doOperationQueueWork drives the queue: worked "
            "again right away after progress, asleep while a transfer runs until it wakes the queue.\n"
            "- **libssh sftp_aio**: libssh directly on the same kind of connection, one file after another with 8 MiB "
            "of requests in flight.\n"
            "- **OpenSSH sftp**: the host's sftp program in batch mode, its start and login measured with an empty "
            "batch and subtracted.\n"
            "\n"
            "Single files of 16 KiB or more go through the asynchronous transfer, smaller ones through one blocking "
            "call per work(). Directories are bulk transfers that scan first, the scan is part of the time. The "
            "local files are in the page cache, so the host's disk hardly takes part.\n";
        if (!context.notes.empty())
        {
            report += "\n## Notes\n\n";
            for (auto const& note : context.notes)
                report += fmt::format("- {}\n", note);
        }
        return report;
    }
}
