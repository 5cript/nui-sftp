#pragma once

#include <optional>

namespace SharedData
{
    /**
     * @brief The level range a compression library accepts and the library's own default level.
     */
    struct CompressionLevelRange
    {
        int minimum;
        int maximum;
        int defaultLevel;
        /** @brief The first level that needs much more memory to pack and to unpack, if the codec has one. */
        std::optional<int> firstUltraLevel{};
    };

    /** @brief gzip deflate level. */
    inline constexpr CompressionLevelRange gzipLevelRange{.minimum = 1, .maximum = 9, .defaultLevel = 6};

    /** @brief bzip2 block size in 100 kB units. */
    inline constexpr CompressionLevelRange bzip2BlockSizeRange{.minimum = 1, .maximum = 9, .defaultLevel = 9};

    /** @brief zstd level, 20 and above are the ultra levels. */
    inline constexpr CompressionLevelRange zstdLevelRange{
        .minimum = 1,
        .maximum = 22,
        .defaultLevel = 3,
        .firstUltraLevel = 20,
    };

    /** @brief xz preset. */
    inline constexpr CompressionLevelRange xzPresetRange{.minimum = 0, .maximum = 9, .defaultLevel = 6};
}
