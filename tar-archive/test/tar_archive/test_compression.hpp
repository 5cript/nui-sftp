#pragma once

#include <tar_archive/compression.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

extern std::filesystem::path programDirectory;

namespace TarArchive::Test
{
    namespace
    {
        std::filesystem::path compressionScratchDirectory()
        {
            const auto directory = programDirectory / "temp" / "tar_archive_compression";
            std::filesystem::create_directories(directory);
            return directory;
        }

        std::vector<std::byte> generatePayload(std::size_t size, std::uint64_t seed)
        {
            std::mt19937_64 engine{seed};
            std::vector<std::byte> payload(size);
            for (auto& datum : payload)
                datum = static_cast<std::byte>(engine() & 0xFFu);
            return payload;
        }

        std::expected<void, TarError>
        writeAllThroughSink(ByteSink& sink, std::span<std::byte const> payload)
        {
            const std::size_t chunkSize = 17u * 1024u;
            std::size_t offset = 0u;
            while (offset < payload.size())
            {
                const std::size_t size = std::min(chunkSize, payload.size() - offset);
                const auto written = sink.write(payload.subspan(offset, size));
                if (!written)
                    return std::unexpected(written.error());
                offset += size;
            }
            return sink.finish();
        }

        std::expected<std::vector<std::byte>, TarError>
        readAllThroughSource(ByteSource& source)
        {
            std::vector<std::byte> accumulator;
            std::array<std::byte, 8192u> scratch{};
            while (!source.eof())
            {
                const auto produced = source.read(scratch);
                if (!produced)
                    return std::unexpected(produced.error());
                if (*produced == 0u)
                    break;
                accumulator.insert(
                    accumulator.end(),
                    scratch.begin(),
                    scratch.begin() + static_cast<std::ptrdiff_t>(*produced)
                );
            }
            return accumulator;
        }

        void runRoundTrip(Compression codec, std::string const& filename)
        {
            const auto directory = compressionScratchDirectory();
            const auto path = directory / filename;

            const std::vector<std::byte> payload = generatePayload(250u * 1024u, 0xC0FFEEu);

            {
                auto sink = makeSink(path, codec);
                ASSERT_TRUE(sink.has_value()) << sink.error().toString();
                const auto written = writeAllThroughSink(*sink.value(), payload);
                ASSERT_TRUE(written.has_value()) << written.error().toString();
            }

            auto source = makeSource(path, codec);
            ASSERT_TRUE(source.has_value()) << source.error().toString();
            const auto readBack = readAllThroughSource(*source.value());
            ASSERT_TRUE(readBack.has_value()) << readBack.error().toString();

            EXPECT_EQ(*readBack, payload);
        }
    }

    TEST(Compression, RawPassThroughRoundTrip)
    {
        runRoundTrip(Compression::None, "raw.bin");
    }

    TEST(Compression, GzipRoundTrip)
    {
        runRoundTrip(Compression::Gzip, "data.gz");
    }

    TEST(Compression, Bzip2RoundTrip)
    {
        runRoundTrip(Compression::Bzip2, "data.bz2");
    }

    TEST(Compression, ZstdRoundTrip)
    {
        runRoundTrip(Compression::Zstd, "data.zst");
    }

#ifdef TAR_ARCHIVE_WITH_XZ
    TEST(Compression, XzRoundTrip)
    {
        runRoundTrip(Compression::Xz, "data.xz");
    }
#endif

    TEST(Compression, ExtensionMappingCoversCommonSuffixes)
    {
        EXPECT_EQ(compressionFromExtension("archive.tar"), Compression::None);
        EXPECT_EQ(compressionFromExtension("archive.tar.gz"), Compression::Gzip);
        EXPECT_EQ(compressionFromExtension("archive.tgz"), Compression::Gzip);
        EXPECT_EQ(compressionFromExtension("archive.tar.bz2"), Compression::Bzip2);
        EXPECT_EQ(compressionFromExtension("archive.tbz2"), Compression::Bzip2);
        EXPECT_EQ(compressionFromExtension("archive.tar.zst"), Compression::Zstd);
        EXPECT_EQ(compressionFromExtension("archive.tzst"), Compression::Zstd);
        EXPECT_EQ(compressionFromExtension("archive.tar.xz"), Compression::Xz);
    }

    TEST(Compression, MagicDetectionAgreesWithWrittenCodec)
    {
        const auto directory = compressionScratchDirectory();
        struct Sample
        {
            Compression codec;
            std::filesystem::path path;
        };
        const std::array<Sample, 3u> samples{{
            {Compression::Gzip, directory / "probe.gz"},
            {Compression::Bzip2, directory / "probe.bz2"},
            {Compression::Zstd, directory / "probe.zst"},
        }};
        const auto payload = generatePayload(4096u, 0xABCDu);
        for (auto const& sample : samples)
        {
            {
                auto sink = makeSink(sample.path, sample.codec);
                ASSERT_TRUE(sink.has_value());
                ASSERT_TRUE(writeAllThroughSink(*sink.value(), payload).has_value());
            }
            const auto detected = detectCompressionByMagic(sample.path);
            ASSERT_TRUE(detected.has_value()) << detected.error().toString();
            EXPECT_EQ(*detected, sample.codec);
        }
    }
}
