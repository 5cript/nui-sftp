#include <ftp/support/tree_responder.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <system_error>
#include <utility>

namespace FileTransferProtocol::Test
{
    namespace
    {
        using Chain = std::vector<ServedEntry*>;

        /** @brief Links followed in one lookup before it fails, as ELOOP does. */
        constexpr int maximumHops = 16;

        std::vector<std::string_view> segmentsOf(std::string_view path)
        {
            std::vector<std::string_view> segments{};
            std::size_t start = 0u;
            while (start <= path.size())
            {
                const auto end = std::min(path.find('/', start), path.size());
                if (end > start)
                    segments.push_back(path.substr(start, end - start));
                start = end + 1u;
            }
            return segments;
        }

        ServedEntry* childNamed(ServedEntry& directory, std::string_view name)
        {
            const auto found = std::ranges::find(directory.children, name, &ServedEntry::name);
            if (found == directory.children.end())
                return nullptr;
            return &*found;
        }

        /**
         * @brief The entries from the root to what @p path leads to from @p chain. The last segment is followed when
         *        it is a link and @p followLast.
         */
        std::optional<Chain> walk(Chain chain, std::string_view path, bool followLast, int& hops)
        {
            if (path.starts_with('/'))
                chain.resize(1u);
            const auto segments = segmentsOf(path);
            for (std::size_t index = 0u; index < segments.size(); ++index)
            {
                const auto segment = segments[index];
                const bool last = index + 1u == segments.size();
                if (segment == ".")
                    continue;
                if (segment == "..")
                {
                    if (chain.size() > 1u)
                        chain.pop_back();
                    continue;
                }
                if (chain.back()->type != ServedEntry::Type::Directory)
                    return std::nullopt;
                auto* child = childNamed(*chain.back(), segment);
                if (child == nullptr)
                    return std::nullopt;
                if (child->type != ServedEntry::Type::Link || (last && !followLast))
                {
                    chain.push_back(child);
                    continue;
                }
                if (++hops > maximumHops)
                    return std::nullopt;
                auto resolved = walk(chain, child->target, true, hops);
                if (!resolved)
                    return std::nullopt;
                chain = std::move(*resolved);
            }
            return chain;
        }

        std::string pathOf(Chain const& chain)
        {
            std::string path{};
            for (std::size_t index = 1u; index < chain.size(); ++index)
                path += fmt::format("/{}", chain[index]->name);
            if (path.empty())
                return "/";
            return path;
        }

        std::string listedLine(ServedEntry const& entry)
        {
            switch (entry.type)
            {
                case ServedEntry::Type::File:
                    return fmt::format(
                        "-rw-r--r--    1 0        0        {:>8} Jan 02 03:04 {}\r\n", entry.content.size(), entry.name
                    );
                case ServedEntry::Type::Directory:
                    return fmt::format("drwxr-xr-x    2 0        0            4096 Jan 02 03:04 {}\r\n", entry.name);
                case ServedEntry::Type::Link:
                    return fmt::format(
                        "lrwxrwxrwx    1 0        0        {:>8} Jan 02 03:04 {} -> {}\r\n",
                        entry.target.size(),
                        entry.name,
                        entry.target
                    );
            }
            return {};
        }

        std::pair<std::string_view, std::string_view> verbAndArgument(std::string_view line)
        {
            const auto space = line.find(' ');
            if (space == std::string_view::npos)
                return {line, {}};
            return {line.substr(0u, space), line.substr(space + 1u)};
        }

        Script replying(std::string_view reply)
        {
            Script script{};
            script.reply(reply);
            return script;
        }

        std::string_view removalReply(ServedTree::Removal removal, std::string_view done)
        {
            switch (removal)
            {
                case ServedTree::Removal::Removed:
                    return done;
                case ServedTree::Removal::Missing:
                    return "550 No such file or directory";
                case ServedTree::Removal::WrongType:
                    return "550 Not the right type.";
                case ServedTree::Removal::NotEmpty:
                    return "550 Directory not empty.";
            }
            return "550 Failed.";
        }
    }

    ServedEntry servedFile(std::string name, std::string content)
    {
        return ServedEntry{.name = std::move(name), .type = ServedEntry::Type::File, .content = std::move(content)};
    }

    ServedEntry servedDirectory(std::string name, std::vector<ServedEntry> children)
    {
        return ServedEntry{
            .name = std::move(name),
            .type = ServedEntry::Type::Directory,
            .children = std::move(children),
        };
    }

    ServedEntry servedLink(std::string name, std::string target)
    {
        return ServedEntry{.name = std::move(name), .type = ServedEntry::Type::Link, .target = std::move(target)};
    }

    ServedTree::ServedTree(std::vector<ServedEntry> root)
        : root_{servedDirectory({}, std::move(root))}
    {}

    std::optional<std::string> ServedTree::directoryAt(std::string_view path)
    {
        std::scoped_lock lock{mutex_};
        int hops{0};
        const auto chain = walk(Chain{&root_}, path, true, hops);
        if (!chain || chain->back()->type != ServedEntry::Type::Directory)
            return std::nullopt;
        return pathOf(*chain);
    }

    std::optional<std::string> ServedTree::listingOf(std::string_view path)
    {
        std::scoped_lock lock{mutex_};
        int hops{0};
        const auto chain = walk(Chain{&root_}, path, true, hops);
        if (!chain || chain->back()->type != ServedEntry::Type::Directory)
            return std::nullopt;
        listed_.push_back(pathOf(*chain));
        std::string lines{};
        for (auto const& child : chain->back()->children)
            lines += listedLine(child);
        return lines;
    }

    std::optional<std::string> ServedTree::contentOf(std::string_view path)
    {
        std::scoped_lock lock{mutex_};
        int hops{0};
        const auto chain = walk(Chain{&root_}, path, true, hops);
        if (!chain || chain->back()->type != ServedEntry::Type::File)
            return std::nullopt;
        return chain->back()->content;
    }

    std::optional<std::string> ServedTree::retrieve(std::string_view path)
    {
        std::scoped_lock lock{mutex_};
        int hops{0};
        const auto chain = walk(Chain{&root_}, path, true, hops);
        if (!chain || chain->back()->type != ServedEntry::Type::File)
            return std::nullopt;
        retrieved_.push_back(pathOf(*chain));
        return chain->back()->content;
    }

    ServedTree::Removal ServedTree::removeFile(std::string_view path)
    {
        return remove(path, ServedEntry::Type::File);
    }

    ServedTree::Removal ServedTree::removeDirectory(std::string_view path)
    {
        return remove(path, ServedEntry::Type::Directory);
    }

    ServedTree::Removal ServedTree::remove(std::string_view path, ServedEntry::Type type)
    {
        std::scoped_lock lock{mutex_};
        int hops{0};
        const auto chain = walk(Chain{&root_}, path, false, hops);
        if (!chain || chain->size() < 2u)
            return Removal::Missing;
        auto* entry = chain->back();
        const bool isDirectory = entry->type == ServedEntry::Type::Directory;
        if ((type == ServedEntry::Type::Directory) != isDirectory)
            return Removal::WrongType;
        if (isDirectory && !entry->children.empty())
            return Removal::NotEmpty;
        auto& siblings = (*chain)[chain->size() - 2u]->children;
        const auto position = std::ranges::find_if(
            siblings,
            [entry](ServedEntry const& sibling)
            {
                return &sibling == entry;
            }
        );
        siblings.erase(position);
        removed_.emplace_back(path);
        return Removal::Removed;
    }

    bool ServedTree::exists(std::string_view path)
    {
        std::scoped_lock lock{mutex_};
        int hops{0};
        return walk(Chain{&root_}, path, false, hops).has_value();
    }

    std::vector<std::string> ServedTree::listed() const
    {
        std::scoped_lock lock{mutex_};
        return listed_;
    }

    std::vector<std::string> ServedTree::removed() const
    {
        std::scoped_lock lock{mutex_};
        return removed_;
    }

    std::vector<std::string> ServedTree::retrieved() const
    {
        std::scoped_lock lock{mutex_};
        return retrieved_;
    }

    TreeResponder::TreeResponder(std::shared_ptr<ServedTree> tree)
        : tree_{std::move(tree)}
    {}

    std::string TreeResponder::absolute(std::string_view path) const
    {
        if (path.starts_with('/'))
            return std::string{path};
        if (workingDirectory_ == "/")
            return fmt::format("/{}", path);
        return fmt::format("{}/{}", workingDirectory_, path);
    }

    Script TreeResponder::answer(std::string_view line)
    {
        const auto [verb, argument] = verbAndArgument(line);
        if (verb == "USER")
            return replying("331 Please specify the password.");
        if (verb == "PASS")
            return replying("230 Login successful.");
        if (verb == "FEAT")
            return replying("211-Features:\n EPSV\n MDTM\n PASV\n REST STREAM\n SIZE\n UTF8\n211 End");
        if (verb == "OPTS" || verb == "TYPE" || verb == "NOOP")
            return replying("200 Command okay.");
        if (verb == "PWD")
            return replying(fmt::format("257 \"{}\" is the current directory", workingDirectory_));
        if (verb == "CWD")
        {
            const auto directory = tree_->directoryAt(absolute(argument));
            if (!directory)
                return replying("550 Failed to change directory.");
            workingDirectory_ = *directory;
            return replying("250 Directory successfully changed.");
        }
        if (verb == "EPSV" || verb == "PASV")
        {
            passiveOpen_ = true;
            Script script{};
            script.passive(DataPlan{});
            return script;
        }
        if (verb == "LIST")
        {
            if (!passiveOpen_)
                return replying("425 Use PORT or PASV first.");
            passiveOpen_ = false;
            // The client lists the working directory, an argument is only -a.
            const auto lines = tree_->listingOf(workingDirectory_);
            Script script{};
            script.reply("150 Here comes the directory listing.")
                .beginData(DataPlan{}.send(lines.value_or("")))
                .awaitData()
                .reply("226 Directory send OK.");
            return script;
        }
        if (verb == "SIZE" || verb == "MDTM")
        {
            const auto content = tree_->contentOf(absolute(argument));
            if (!content)
                return replying("550 Could not get file size.");
            if (verb == "MDTM")
                return replying("213 20260102030405");
            return replying(fmt::format("213 {}", content->size()));
        }
        if (verb == "REST")
        {
            const auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(), restartAt_);
            if (parsed.ec != std::errc{} || parsed.ptr != argument.data() + argument.size())
                return replying("501 Syntax error in parameters or arguments.");
            return replying(fmt::format("350 Restart position accepted ({}).", restartAt_));
        }
        if (verb == "RETR")
        {
            const auto offset = std::exchange(restartAt_, 0u);
            if (!passiveOpen_)
                return replying("425 Use PORT or PASV first.");
            passiveOpen_ = false;
            const auto content = tree_->retrieve(absolute(argument));
            if (!content)
                return replying("550 Failed to open file.");
            Script script{};
            script.reply(fmt::format("150 Opening BINARY mode data connection ({} bytes).", content->size()))
                .beginData(DataPlan{}.send(content->substr(std::min<std::size_t>(offset, content->size()))))
                .awaitData()
                .reply("226 Transfer complete.");
            return script;
        }
        if (verb == "DELE")
            return replying(removalReply(tree_->removeFile(absolute(argument)), "250 Delete operation successful."));
        if (verb == "RMD")
            return replying(
                removalReply(tree_->removeDirectory(absolute(argument)), "250 Remove directory operation successful.")
            );
        if (verb == "ABOR")
            return replying("225 No transfer to ABOR.");
        if (verb == "QUIT")
        {
            Script script{};
            script.reply("221 Goodbye.").closeControl();
            return script;
        }
        if (verb == "STAT" || verb == "MLSD" || verb == "MLST")
            return replying("500 Unknown command.");
        if (verb == "MKD" || verb == "STOR" || verb == "APPE" || verb == "RNFR" || verb == "SITE" || verb == "MFMT")
            return replying("550 Permission denied.");
        return replying("502 Command not implemented.");
    }

    Script serving(std::shared_ptr<ServedTree> tree)
    {
        Script script{};
        script.reply("220 Served").serve(std::make_shared<TreeResponder>(std::move(tree)));
        return script;
    }
}
