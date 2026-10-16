#pragma once

#include <utility/format_bytes.hpp>
#include <utility/language.hpp>

#include <nui/event_system/observed_value.hpp>

#include <fmt/format.h>

#include <cstdint>
#include <string>

/**
 * @brief Scan progress of a card whose operation scans before its own work.
 */
struct ScanPhaseStatus
{
    explicit ScanPhaseStatus(bool isScanning)
        : scanning{isScanning}
    {}

    void setProgress(std::uint64_t scannedBytes, std::uint64_t scannedEntries)
    {
        // Scan progress can arrive after the operation already moved on.
        if (!scanning.value())
            return;
        if (totalBytes.value() != scannedBytes)
            totalBytes = scannedBytes;
        if (totalScanned.value() != scannedEntries)
            totalScanned = scannedEntries;
    }

    void end()
    {
        if (scanning.value())
            scanning = false;
    }

    std::string text() const
    {
        return fmt::format(
            fmt::runtime(language->get("operationQueue", "scanningItems")),
            totalScanned.value(),
            Utility::formatBytes(static_cast<long long>(totalBytes.value()))
        );
    }

    Nui::Observed<bool> scanning;
    Nui::Observed<std::uint64_t> totalBytes{0u};
    Nui::Observed<std::uint64_t> totalScanned{0u};
};

/**
 * @brief The part of a card whose operation scans before its own work: the scan status and its progress setter.
 */
class ScanningCard
{
  public:
    explicit ScanningCard(bool scanning)
        : scanPhase_{scanning}
    {}

    void setScanProgress(std::uint64_t scannedBytes, std::uint64_t scannedEntries)
    {
        scanPhase_.setProgress(scannedBytes, scannedEntries);
        Nui::globalEventContext.executeActiveEventsImmediately();
    }

  protected:
    ScanPhaseStatus scanPhase_;
};
