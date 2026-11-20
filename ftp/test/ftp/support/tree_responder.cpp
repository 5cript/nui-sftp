#include <ftp/support/tree_responder.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <array>
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

        /** @brief The most bytes one upload may send. */
        constexpr std::size_t uploadLimit = 256u * 1024u * 1024u;

        /** @brief What MDTM tells of a file whose time was never set. */
        constexpr std::string_view defaultModified{"20260102030405"};

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

        /**
         * @brief The directory that holds the absolute @p path and its last segment.
         */
        std::pair<std::string_view, std::string_view> splitLast(std::string_view path)
        {
            while (path.size() > 1u && path.ends_with('/'))
                path.remove_suffix(1u);
            const auto slash = path.find_last_of('/');
            if (slash == std::string_view::npos)
                return {".", path};
            if (slash == 0u)
                return {"/", path.substr(1u)};
            return {path.substr(0u, slash), path.substr(slash + 1u)};
        }

        bool namesAnEntry(std::string_view name)
        {
            return !name.empty() && name != "." && name != "..";
        }

        /**
         * @brief The entries from the root to the directory that would hold @p path, where @p path names nothing yet.
         */
        std::optional<Chain> creatableAt(ServedEntry& root, std::string_view path)
        {
            const auto [directory, name] = splitLast(path);
            int hops{0};
            auto parent = walk(Chain{&root}, directory, true, hops);
            if (!parent || parent->back()->type != ServedEntry::Type::Directory || !namesAnEntry(name) ||
                childNamed(*parent->back(), name) != nullptr)
                return std::nullopt;
            return parent;
        }

        /**
         * @brief Whether @p moved may replace @p replaced in a rename.
         */
        bool replaceable(ServedEntry const& moved, ServedEntry const& replaced)
        {
            const bool replacesDirectory = replaced.type == ServedEntry::Type::Directory;
            if (moved.type != ServedEntry::Type::Directory)
                return !replacesDirectory;
            return replacesDirectory && replaced.children.empty();
        }

        /**
         * @brief The time LIST shows for @p entry, the same for every entry whose time was never set.
         */
        std::string listedTimeOf(ServedEntry const& entry)
        {
            constexpr std::array<std::string_view, 12u> months{
                "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
            };
            if (!entry.modified || entry.modified->size() != 14u)
                return "Jan 02 03:04";
            std::string_view const time{*entry.modified};
            unsigned month{0u};
            std::from_chars(time.data() + 4, time.data() + 6, month);
            if (month < 1u || month > months.size())
                return "Jan 02 03:04";
            return fmt::format("{} {}  {}", months[month - 1u], time.substr(6u, 2u), time.substr(0u, 4u));
        }

        /**
         * @brief Whether @p time is a time-val of MFMT without fractions.
         */
        bool isTimeVal(std::string_view time)
        {
            const auto isDigit = [](char character)
            {
                return character >= '0' && character <= '9';
            };
            if (time.size() != 14u || !std::ranges::all_of(time, isDigit))
                return false;
            const auto month = (time[4] - '0') * 10 + (time[5] - '0');
            return month >= 1 && month <= 12;
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
                        "-{}    1 0        0        {:>8} {} {}\r\n",
                        entry.permissions,
                        entry.content.size(),
                        listedTimeOf(entry),
                        entry.name
                    );
                case ServedEntry::Type::Directory:
                    return fmt::format(
                        "drwxr-xr-x    2 0        0            4096 {} {}\r\n", listedTimeOf(entry), entry.name
                    );
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

    std::optional<std::pair<ServedEntry*, std::string>> ServedTree::fileAt(std::string_view path)
    {
        int hops{0};
        if (const auto chain = walk(Chain{&root_}, path, true, hops))
        {
            if (chain->back()->type != ServedEntry::Type::File)
                return std::nullopt;
            return std::pair{chain->back(), pathOf(*chain)};
        }
        auto parent = creatableAt(root_, path);
        if (!parent)
            return std::nullopt;
        auto& directory = *parent->back();
        directory.children.push_back(servedFile(std::string{splitLast(path).second}, {}));
        parent->push_back(&directory.children.back());
        return std::pair{parent->back(), pathOf(*parent)};
    }

    std::optional<std::uint64_t> ServedTree::openForUpload(std::string_view path, Upload upload, std::uint64_t offset)
    {
        std::scoped_lock lock{mutex_};
        auto file = fileAt(path);
        if (!file)
            return std::nullopt;
        auto& [entry, resolved] = *file;
        stored_.push_back(std::move(resolved));
        entry->modified.reset();
        if (upload == Upload::Append)
            return entry->content.size();
        if (offset == 0u)
            entry->content.clear();
        return offset;
    }

    bool ServedTree::write(std::string_view path, std::uint64_t position, std::string_view bytes)
    {
        std::scoped_lock lock{mutex_};
        int hops{0};
        const auto chain = walk(Chain{&root_}, path, true, hops);
        if (!chain || chain->back()->type != ServedEntry::Type::File)
            return false;
        auto& content = chain->back()->content;
        const auto end = static_cast<std::size_t>(position) + bytes.size();
        if (content.size() < end)
            content.resize(end, '\0');
        std::ranges::copy(bytes, content.begin() + static_cast<std::ptrdiff_t>(position));
        return true;
    }

    bool ServedTree::putFile(std::string_view path, std::string content)
    {
        std::scoped_lock lock{mutex_};
        const auto file = fileAt(path);
        if (!file)
            return false;
        file->first->content = std::move(content);
        file->first->modified.reset();
        return true;
    }

    bool ServedTree::makeDirectory(std::string_view path)
    {
        std::scoped_lock lock{mutex_};
        const auto parent = creatableAt(root_, path);
        if (!parent)
            return false;
        parent->back()->children.push_back(servedDirectory(std::string{splitLast(path).second}, {}));
        return true;
    }

    bool ServedTree::rename(std::string_view from, std::string_view to)
    {
        std::scoped_lock lock{mutex_};
        int hops{0};
        const auto source = walk(Chain{&root_}, from, false, hops);
        if (!source || source->size() < 2u)
            return false;
        const auto [directory, name] = splitLast(to);
        hops = 0;
        const auto target = walk(Chain{&root_}, directory, true, hops);
        if (!target || target->back()->type != ServedEntry::Type::Directory || !namesAnEntry(name))
            return false;
        auto* moved = source->back();
        if (std::ranges::find(*target, moved) != target->end())
            return false;
        auto const* replaced = childNamed(*target->back(), name);
        if (replaced == moved)
            return true;
        if (replaced != nullptr && !replaceable(*moved, *replaced))
            return false;

        // Taking the entry out moves its later siblings, the target is looked up again behind it.
        ServedEntry entry = std::move(*moved);
        entry.name = std::string{name};
        auto& siblings = (*source)[source->size() - 2u]->children;
        siblings.erase(siblings.begin() + (moved - siblings.data()));
        hops = 0;
        const auto again = walk(Chain{&root_}, directory, true, hops);
        if (!again)
            return false;
        auto& children = again->back()->children;
        if (auto* existing = childNamed(*again->back(), name))
            *existing = std::move(entry);
        else
            children.push_back(std::move(entry));
        return true;
    }

    bool ServedTree::setModified(std::string_view path, std::string time)
    {
        std::scoped_lock lock{mutex_};
        int hops{0};
        const auto chain = walk(Chain{&root_}, path, true, hops);
        if (!chain)
            return false;
        chain->back()->modified = std::move(time);
        return true;
    }

    std::optional<std::string> ServedTree::modifiedOf(std::string_view path)
    {
        std::scoped_lock lock{mutex_};
        int hops{0};
        const auto chain = walk(Chain{&root_}, path, true, hops);
        if (!chain || chain->back()->type != ServedEntry::Type::File)
            return std::nullopt;
        return chain->back()->modified.value_or(std::string{defaultModified});
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

    std::vector<std::string> ServedTree::stored() const
    {
        std::scoped_lock lock{mutex_};
        return stored_;
    }

    TreeResponder::TreeResponder(std::shared_ptr<ServedTree> tree, ServedFeatures features)
        : tree_{std::move(tree)}
        , features_{features}
    {}

    std::string TreeResponder::absolute(std::string_view path) const
    {
        if (path.starts_with('/'))
            return std::string{path};
        if (workingDirectory_ == "/")
            return fmt::format("/{}", path);
        return fmt::format("{}/{}", workingDirectory_, path);
    }

    Script TreeResponder::upload(std::string_view verb, std::string_view argument)
    {
        const auto offset = std::exchange(restartAt_, 0u);
        if (!passiveOpen_)
            return replying("425 Use PORT or PASV first.");
        passiveOpen_ = false;
        const auto path = absolute(argument);
        const auto upload = verb == "APPE" ? ServedTree::Upload::Append : ServedTree::Upload::Store;
        const auto position = tree_->openForUpload(path, upload, offset);
        if (!position)
            return replying("553 Could not create file.");
        auto written = std::make_shared<std::uint64_t>(*position);
        Script script{};
        script.reply("150 Ok to send data.")
            .beginData(
                DataPlan{}.expectUpload(
                    uploadLimit,
                    [tree = tree_, path, written](std::string_view bytes)
                    {
                        tree->write(path, *written, bytes);
                        *written += bytes.size();
                    }
                )
            )
            .awaitData()
            .reply("226 Transfer complete.");
        return script;
    }

    Script TreeResponder::renameTo(std::optional<std::string> from, std::string_view argument)
    {
        if (!from)
            return replying("503 RNFR required first.");
        if (!tree_->rename(*from, absolute(argument)))
            return replying("550 Rename failed.");
        return replying("250 Rename successful.");
    }

    Script TreeResponder::setModified(std::string_view argument)
    {
        const auto [time, path] = verbAndArgument(argument);
        if (!isTimeVal(time))
            return replying("501 Syntax error in parameters or arguments.");
        if (!tree_->setModified(absolute(path), std::string{time}))
            return replying("550 Could not set file modification time.");
        return replying(fmt::format("213 Modify={}; {}", time, path));
    }

    Script TreeResponder::answer(std::string_view line)
    {
        const auto [verb, argument] = verbAndArgument(line);
        // RNTO takes the RNFR right before it.
        auto renameFrom = std::exchange(renameFrom_, std::nullopt);
        if (verb == "USER")
            return replying("331 Please specify the password.");
        if (verb == "PASS")
            return replying("230 Login successful.");
        if (verb == "FEAT")
            return replying(
                fmt::format(
                    "211-Features:\n EPSV\n MDTM\n MFMT\n PASV\n REST STREAM\n{}UTF8\n211 End",
                    features_.size ? " SIZE\n " : " "
                )
            );
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
        if (verb == "SIZE" && features_.size)
        {
            const auto content = tree_->contentOf(absolute(argument));
            if (!content)
                return replying("550 Could not get file size.");
            return replying(fmt::format("213 {}", content->size()));
        }
        if (verb == "MDTM")
        {
            const auto time = tree_->modifiedOf(absolute(argument));
            if (!time)
                return replying("550 Could not get file modification time.");
            return replying(fmt::format("213 {}", *time));
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
        if (verb == "STOR" || verb == "APPE")
            return upload(verb, argument);
        if (verb == "MKD")
        {
            const auto path = absolute(argument);
            if (!tree_->makeDirectory(path))
                return replying("550 Create directory operation failed.");
            return replying(fmt::format("257 \"{}\" created", path));
        }
        if (verb == "RNFR")
        {
            if (!tree_->exists(absolute(argument)))
                return replying("550 RNFR command failed.");
            renameFrom_ = absolute(argument);
            return replying("350 Ready for RNTO.");
        }
        if (verb == "RNTO")
            return renameTo(std::move(renameFrom), argument);
        if (verb == "MFMT")
            return setModified(argument);
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
        if (verb == "SITE")
            return replying("550 Permission denied.");
        return replying("502 Command not implemented.");
    }

    Script serving(std::shared_ptr<ServedTree> tree, ServedFeatures features)
    {
        Script script{};
        script.reply("220 Served").serve(std::make_shared<TreeResponder>(std::move(tree), features));
        return script;
    }
}
