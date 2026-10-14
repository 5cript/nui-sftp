#include <utility/no_follow_tree_walk.hpp>
#include "no_follow_inspection.hpp"

#include <utility>

namespace Utility
{
    namespace
    {
        Detail::NoFollowInspection inspectEntry(std::filesystem::directory_entry const& entry, std::error_code& error)
        {
#ifdef _WIN32
            // The cached type does not tell a junction from a directory, the reparse tag does.
            return Detail::inspectNoFollow(entry.path(), error);
#else
            const auto status = entry.symlink_status(error);
            if (status.type() == std::filesystem::file_type::not_found)
                error.clear();
            if (error)
                return {};
            return Detail::inspectionOf(status);
#endif
        }
    }

    NoFollowTreeWalk::NoFollowTreeWalk(std::filesystem::path root, Mode mode)
        : root_{std::move(root)}
        , mode_{mode}
    {}

    bool NoFollowTreeWalk::step(std::chrono::steady_clock::time_point deadline)
    {
        if (done_)
            return true;
        if (!started_)
        {
            started_ = true;
            start();
        }
        do
        {
            if (stack_.empty())
            {
                done_ = true;
                return true;
            }
            auto& frame = stack_.back();
            if (frame.iterator == std::filesystem::directory_iterator{})
            {
                finishDirectory();
                continue;
            }
            const auto entry = *frame.iterator;
            std::error_code error{};
            frame.iterator.increment(error);
            if (error)
            {
                fail(frame.directory, error);
                frame.iterator = std::filesystem::directory_iterator{};
            }
            handleEntry(entry);
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }

    void NoFollowTreeWalk::start()
    {
        std::error_code error{};
        const auto inspection = Detail::inspectNoFollow(root_, error);
        if (error)
            return fail(root_, error);
        switch (inspection.kind)
        {
            case NoFollowKind::Missing:
                return;
            case NoFollowKind::Directory:
                return enter(root_);
            case NoFollowKind::Link:
            case NoFollowKind::Other:
            {
                const auto size = mode_ == Mode::Count && inspection.kind == NoFollowKind::Other
                    ? std::filesystem::file_size(root_, error)
                    : std::uintmax_t{0};
                return finishItem(root_, inspection.kind, inspection.directoryEntry, error ? 0 : size);
            }
        }
    }

    void NoFollowTreeWalk::handleEntry(std::filesystem::directory_entry const& entry)
    {
        std::error_code error{};
        const auto inspection = inspectEntry(entry, error);
        if (error)
            return fail(entry.path(), error);
        switch (inspection.kind)
        {
            case NoFollowKind::Missing:
                return;
            case NoFollowKind::Directory:
                return enter(entry.path());
            case NoFollowKind::Link:
            case NoFollowKind::Other:
            {
                const bool countsBytes = mode_ == Mode::Count && inspection.kind == NoFollowKind::Other &&
                    entry.is_regular_file(error);
                const auto size = countsBytes ? entry.file_size(error) : std::uintmax_t{0};
                return finishItem(entry.path(), inspection.kind, inspection.directoryEntry, error ? 0 : size);
            }
        }
    }

    void NoFollowTreeWalk::enter(std::filesystem::path const& directory)
    {
        lastPath_ = directory;
        std::error_code error{};
        std::filesystem::directory_iterator iterator{directory, error};
        if (error)
        {
            if (mode_ == Mode::Count)
            {
                ++entries_;
                return;
            }
            return fail(directory, error);
        }
        stack_.push_back(Frame{.directory = directory, .iterator = std::move(iterator)});
    }

    void NoFollowTreeWalk::finishDirectory()
    {
        auto frame = std::move(stack_.back());
        stack_.pop_back();
        if (frame.keepsContent)
        {
            if (!stack_.empty())
                stack_.back().keepsContent = true;
            return;
        }
        finishItem(frame.directory, NoFollowKind::Directory, true, 0);
    }

    void NoFollowTreeWalk::finishItem(
        std::filesystem::path const& path,
        NoFollowKind kind,
        bool directoryEntry,
        std::uintmax_t size
    )
    {
        lastPath_ = path;
        if (mode_ == Mode::Count)
        {
            ++entries_;
            bytes_ += size;
            return;
        }
        std::error_code error{};
        if (Detail::removeInspected(path, {.kind = kind, .directoryEntry = directoryEntry}, error))
            ++entries_;
        else if (error)
            fail(path, error);
    }

    void NoFollowTreeWalk::fail(std::filesystem::path const& path, std::error_code const& error)
    {
        lastPath_ = path;
        if (mode_ == Mode::Count)
            return;
        failures_.push_back({.path = path, .error = error});
        if (!stack_.empty())
            stack_.back().keepsContent = true;
    }
}
