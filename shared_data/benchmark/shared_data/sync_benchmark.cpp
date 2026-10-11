#include <shared_data/sync/diff.hpp>
#include <shared_data/sync/diff_tree_store.hpp>
#include <shared_data/sync/enqueue_minimizer.hpp>
#include <shared_data/sync/tree_directory_walker.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <expected>
#include <functional>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

using namespace SharedData::Sync;

namespace
{
    ScanNode makeFile(std::string name, std::uint64_t size, std::uint64_t mtime)
    {
        return ScanNode{.name = std::move(name), .type = SharedData::FileType::Regular, .size = size, .mtime = mtime};
    }

    /** @brief Sorts children and fills subtree metrics like the walker does. */
    ScanNode makeDirectory(std::string name, std::vector<ScanNode> children)
    {
        std::sort(
            children.begin(),
            children.end(),
            [](auto const& lhs, auto const& rhs)
            {
                return lhs.name < rhs.name;
            }
        );
        ScanNode node{
            .name = std::move(name), .type = SharedData::FileType::Directory, .children = std::move(children)
        };
        for (auto const& child : node.children)
        {
            node.subtreeFileCount += child.type == SharedData::FileType::Directory ? child.subtreeFileCount : 1u;
            node.subtreeByteTotal +=
                child.type == SharedData::FileType::Directory ? child.subtreeByteTotal : child.size;
        }
        return node;
    }

    /**
     * @brief Balanced tree with @p fanout directories per level, @p depth levels and @p filesPerDirectory files
     *        in each leaf directory. @p mtimeOf picks the mtime of a file by its running index.
     */
    ScanNode makeBalancedTree(
        std::size_t fanout,
        std::size_t depth,
        std::size_t filesPerDirectory,
        std::function<std::uint64_t(std::size_t)> const& mtimeOf,
        std::size_t& fileIndex,
        std::string name = {}
    )
    {
        std::vector<ScanNode> children;
        if (depth == 0)
        {
            for (std::size_t file = 0; file < filesPerDirectory; ++file)
                children.push_back(makeFile(fmt::format("file{:05}.txt", file), 100, mtimeOf(fileIndex++)));
        }
        else
        {
            for (std::size_t directory = 0; directory < fanout; ++directory)
                children.push_back(makeBalancedTree(
                    fanout, depth - 1, filesPerDirectory, mtimeOf, fileIndex, fmt::format("dir{:03}", directory)
                ));
        }
        return makeDirectory(std::move(name), std::move(children));
    }

    ScanNode balanced(
        std::size_t fanout,
        std::size_t depth,
        std::size_t files,
        std::function<std::uint64_t(std::size_t)> mtimeOf
    )
    {
        std::size_t index = 0;
        return makeBalancedTree(fanout, depth, files, mtimeOf, index);
    }

    struct Stores
    {
        DiffTreeStore uploads;
        DiffTreeStore downloads;
        DiffTreeStore deletes;

        void run(ScanNode const& local, ScanNode const& remote, DiffOptions const& options = {})
        {
            uploads.clear(options);
            downloads.clear(options);
            deletes.clear(options);
            StoreDiffSink sink{uploads, downloads, deletes};
            diffScanTrees(local, remote, options, sink);
            uploads.finalize();
            downloads.finalize();
            deletes.finalize();
        }
    };

    /** @brief Median wall time of @p repetitions runs of @p body in milliseconds. */
    double measure(int repetitions, std::function<void()> const& body)
    {
        std::vector<double> times;
        for (int repetition = 0; repetition < repetitions; ++repetition)
        {
            const auto start = std::chrono::steady_clock::now();
            body();
            times.push_back(
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()
            );
        }
        std::sort(times.begin(), times.end());
        return times[times.size() / 2];
    }

    void report(std::string const& name, std::size_t items, double milliseconds)
    {
        fmt::print(
            "{:<58} {:>9} {:>11.2f} ms {:>9.0f} ns/item\n",
            name,
            items,
            milliseconds,
            milliseconds * 1e6 / static_cast<double>(std::max<std::size_t>(items, 1))
        );
    }
}

int main(int argc, char** argv)
{
    // Scale multiplies the base sizes, e.g. `nui-sftp-sync-benchmark 4`.
    const std::size_t scale = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 1;
    const int repetitions = argc > 2 ? std::atoi(argv[2]) : 5;
    fmt::print("{:<58} {:>9} {:>14} {:>16}\n", "scenario", "items", "median", "per item");

    {
        // 10 x 10 x 10 directories with 20 * scale files each.
        const std::size_t files = 20 * scale;
        std::map<std::string, std::vector<SharedData::DirectoryEntry>> listing;
        std::size_t total = 0;
        const std::function<void(std::string const&, std::size_t)> build =
            [&](std::string const& path, std::size_t depth)
        {
            auto& entries = listing[path];
            if (depth == 3)
            {
                for (std::size_t file = files; file-- > 0;)
                    entries.push_back(
                        {.path = fmt::format("file{:05}.txt", file), .type = SharedData::FileType::Regular, .size = 1}
                    );
                total += files;
                return;
            }
            for (std::size_t directory = 10; directory-- > 0;)
            {
                const auto name = fmt::format("dir{:03}", directory);
                entries.push_back({.path = name, .type = SharedData::FileType::Directory});
                build(path + "/" + name, depth + 1);
            }
        };
        build("/root", 0);
        const auto time = measure(
            repetitions,
            [&]
            {
                auto scanner = [&listing](
                                   std::filesystem::path const& path
                               ) -> std::expected<std::vector<SharedData::DirectoryEntry>, int>
                {
                    return listing.at(path.generic_string());
                };
                TreeDirectoryWalker<int, decltype(scanner), false> walker{"/root", std::move(scanner)};
                if (!walker.walkAll())
                    std::abort();
                auto tree = std::move(walker).ejectTree();
            }
        );
        report("walker: build sorted tree (reverse ordered listing)", total, time);
    }

    {
        const auto tree = balanced(
            10,
            3,
            20 * scale,
            [](std::size_t)
            {
                return 100u;
            }
        );
        Stores stores;
        const auto time = measure(
            repetitions,
            [&]
            {
                stores.run(tree, tree);
            }
        );
        report("diff: identical trees", tree.subtreeFileCount, time);
    }

    const auto local = balanced(
        10,
        3,
        20 * scale,
        [](std::size_t index)
        {
            return index % 2 == 0 ? 200u : 100u;
        }
    );
    const auto remote = balanced(
        10,
        3,
        20 * scale,
        [](std::size_t index)
        {
            return index % 2 == 0 ? 100u : 200u;
        }
    );
    {
        Stores stores;
        const auto time = measure(
            repetitions,
            [&]
            {
                stores.run(local, remote);
            }
        );
        report("diff + finalize: every file differs (half up, half down)", local.subtreeFileCount, time);
    }

    {
        // Many sibling directories with one differing file each: structural rows dominate.
        std::vector<ScanNode> localChildren;
        std::vector<ScanNode> remoteChildren;
        const std::size_t count = 5000 * scale;
        for (std::size_t index = 0; index < count; ++index)
        {
            const auto name = fmt::format("project{:06}", index);
            localChildren.push_back(makeDirectory(
                name, {makeDirectory("src", {makeDirectory("detail", {makeFile("changed.cpp", 2, 200)})})}
            ));
            remoteChildren.push_back(makeDirectory(
                name, {makeDirectory("src", {makeDirectory("detail", {makeFile("changed.cpp", 1, 100)})})}
            ));
        }
        const auto sparseLocal = makeDirectory("", std::move(localChildren));
        const auto sparseRemote = makeDirectory("", std::move(remoteChildren));
        Stores stores;
        const auto time = measure(
            repetitions,
            [&]
            {
                stores.run(sparseLocal, sparseRemote);
            }
        );
        report("diff + finalize: flat siblings, one deep change each", count, time);

        std::unordered_set<std::string> roots;
        for (std::size_t index = 0; index < count; ++index)
            roots.insert(fmt::format("project{:06}", index));
        const ScanTrees trees{.local = &sparseLocal, .remote = &sparseRemote, .localRoot = "/l", .remoteRoot = "/r"};
        const auto planTime = measure(
            repetitions,
            [&]
            {
                auto plan = stores.uploads.buildPlan(roots, trees);
            }
        );
        report("plan: select all, flat siblings", count, planTime);
    }

    {
        Stores stores;
        stores.run(local, remote);
        const ScanTrees trees{.local = &local, .remote = &remote, .localRoot = "/l", .remoteRoot = "/r"};
        std::unordered_set<std::string> roots;
        for (auto const& row : stores.uploads.loadChildren("", trees))
            roots.insert(row.relKey);
        const auto time = measure(
            repetitions,
            [&]
            {
                auto plan = stores.uploads.buildPlan(roots, trees);
            }
        );
        report("plan: select all, every other file uploads", stores.uploads.summary().itemCount, time);
    }

    {
        // Remote is empty: one row, the plan expands the whole subtree.
        const auto empty = makeDirectory("", {});
        std::vector<ScanNode> wrapped;
        wrapped.push_back(local);
        wrapped.back().name = "new_module";
        const auto oneSided = makeDirectory("", std::move(wrapped));
        Stores stores;
        const auto diffTime = measure(
            repetitions,
            [&]
            {
                stores.run(oneSided, empty);
            }
        );
        report("diff + finalize: one-sided subtree", oneSided.subtreeFileCount, diffTime);
        const ScanTrees trees{.local = &oneSided, .remote = &empty, .localRoot = "/l", .remoteRoot = "/r"};
        const auto planTime = measure(
            repetitions,
            [&]
            {
                auto plan = stores.uploads.buildPlan({"new_module"}, trees);
            }
        );
        report("plan: expand one-sided subtree into files", oneSided.subtreeFileCount, planTime);
    }

    {
        std::vector<MinimizerItemView> items;
        const std::size_t count = 50000 * scale;
        items.push_back({.relKey = "dir", .isBulkDir = false});
        for (std::size_t index = 0; index < count; ++index)
            items.push_back({.relKey = fmt::format("dir/sub{:03}/file{:06}", index % 500, index), .isBulkDir = false});
        for (std::size_t index = 0; index < 500; ++index)
            items.push_back({.relKey = fmt::format("dir/sub{:03}", index), .isBulkDir = false});
        const auto time = measure(
            repetitions,
            [&]
            {
                auto result = minimizeEnqueueIndices(items, {"dir"});
            }
        );
        report("minimizer: select all", items.size(), time);
    }

    {
        std::vector<ScanNode> files;
        const std::size_t count = 20000 * scale;
        for (std::size_t index = 0; index < count; ++index)
            files.push_back(makeFile(fmt::format("f{:07}", index), 1, 200));
        const auto flatLocal = makeDirectory("", std::move(files));
        const auto empty = makeDirectory("", {});
        Stores stores;
        stores.run(flatLocal, empty);
        const ScanTrees trees{.local = &flatLocal, .remote = &empty};
        const auto time = measure(
            repetitions,
            [&]
            {
                auto rows = stores.uploads.loadChildren("", trees);
            }
        );
        report("load children: flat root", count, time);
    }
    return 0;
}
