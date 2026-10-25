#include "workload.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <fstream>
#include <random>

namespace Benchmark
{
    namespace
    {
        void writeRandomFile(std::filesystem::path const& path, std::uint64_t size, std::mt19937_64& generator)
        {
            constexpr std::size_t blockSize = 1024u * 1024u;
            std::vector<std::uint64_t> block(blockSize / sizeof(std::uint64_t));
            std::ofstream stream{path, std::ios::binary | std::ios::trunc};
            for (std::uint64_t written = 0; written < size;)
            {
                std::ranges::generate(block, std::ref(generator));
                const auto chunk = std::min<std::uint64_t>(blockSize, size - written);
                stream.write(reinterpret_cast<char const*>(block.data()), static_cast<std::streamsize>(chunk));
                written += chunk;
            }
        }
    }

    std::string_view toString(Direction direction)
    {
        switch (direction)
        {
            case Direction::Download:
                return "download";
            case Direction::Upload:
                return "upload";
        }
        return "unknown";
    }

    std::vector<std::string> createLocalSource(Workload const& workload, std::filesystem::path const& root)
    {
        std::mt19937_64 generator{workload.fileCount * 7919u + workload.fileSize};
        if (workload.isSingleFile())
        {
            std::filesystem::create_directories(root.parent_path());
            writeRandomFile(root, workload.fileSize, generator);
            return {};
        }

        std::vector<std::string> files;
        files.reserve(workload.fileCount);
        for (std::uint64_t index = 0; index < workload.fileCount; ++index)
        {
            const auto relative =
                fmt::format("d{:03}/f{:05}", index / workload.filesPerDirectory, index % workload.filesPerDirectory);
            std::filesystem::create_directories((root / relative).parent_path());
            writeRandomFile(root / relative, workload.fileSize, generator);
            files.push_back(relative);
        }
        std::ranges::sort(files);
        return files;
    }
}
