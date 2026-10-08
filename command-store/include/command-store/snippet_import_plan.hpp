#pragma once

#include <command-store/types.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/**
 * @brief Decides where pasted snippets go and how they relate to the stored ones, before anything
 *        is written; shared by the import dialog and the tests.
 */
namespace CommandStore::SnippetImport
{
    enum class Status
    {
        /** @brief No snippet of that name in the target folder. */
        New,
        /** @brief A snippet of that name with the same command, tags and danger level exists; it is skipped. */
        Identical,
        /** @brief A snippet of that name exists but differs; the user decides. */
        NameConflict,
    };

    enum class Resolution
    {
        Skip,
        Replace,
        KeepBoth,
    };

    /**
     * @brief Where an import goes.
     */
    struct Target
    {
        /**
         * @brief Set: every snippet goes into this folder id, empty meaning the root, and the folder
         *        names of the document are ignored. Unset: folders are taken from the document by
         *        name and created when missing.
         */
        std::optional<std::string> folderId{};
    };

    /**
     * @brief One pasted snippet with its target folder and how it relates to what exists.
     */
    struct PlannedSnippet
    {
        /** @brief The pasted snippet; its folder is the name of the target folder, empty for root. */
        TransferSnippet snippet{};

        /** @brief Id of an existing target folder; empty for root or a folder still to create. */
        std::string folderId{};

        /** @brief The target folder does not exist yet and is created by the import. */
        bool createsFolder{false};

        Status status{Status::New};

        /** @brief The stored snippet of the same name in the target folder, if any. */
        std::optional<Snippet> existing{};

        /** @brief Index of an earlier pasted snippet of the same name and target folder, if any. */
        std::optional<std::size_t> earlierEntry{};

        /**
         * @brief Whether the conflict can be resolved by overwriting a stored snippet; only the first
         *        pasted snippet of a name may, the later ones can only be skipped or kept.
         */
        bool canReplace() const
        {
            return existing.has_value() && !earlierEntry.has_value();
        }
    };

    struct ImportPlan
    {
        std::vector<PlannedSnippet> snippets{};

        /** @brief Folders the import creates, in order of first use. */
        std::vector<std::string> newFolders{};

        /** @brief Folder names of the document that a fixed target folder overrides. */
        std::vector<std::string> ignoredFolders{};

        std::size_t count(Status status) const
        {
            return static_cast<std::size_t>(std::ranges::count_if(
                snippets,
                [status](PlannedSnippet const& planned)
                {
                    return planned.status == status;
                }
            ));
        }
    };

    namespace Detail
    {
        inline bool sameContent(TransferSnippet const& pasted, Snippet const& stored)
        {
            return pasted.command == stored.command && pasted.tags == stored.tags && pasted.danger == stored.danger;
        }

        inline bool sameContent(TransferSnippet const& pasted, TransferSnippet const& earlier)
        {
            return pasted.command == earlier.command && pasted.tags == earlier.tags && pasted.danger == earlier.danger;
        }

        inline void appendUnique(std::vector<std::string>& names, std::string const& name)
        {
            if (std::ranges::find(names, name) == names.end())
                names.push_back(name);
        }

        /**
         * @brief Resolves the target folder of a pasted snippet into planned.
         */
        inline void resolveFolder(
            PlannedSnippet& planned,
            ImportPlan& plan,
            std::vector<SnippetFolder> const& folders,
            Target const& target
        )
        {
            const auto byId = [&folders](std::string const& id)
            {
                return std::ranges::find_if(
                    folders,
                    [&id](SnippetFolder const& folder)
                    {
                        return folder.id == id;
                    }
                );
            };
            const auto byName = [&folders](std::string const& name)
            {
                return std::ranges::find_if(
                    folders,
                    [&name](SnippetFolder const& folder)
                    {
                        return folder.name == name;
                    }
                );
            };

            if (target.folderId)
            {
                if (!planned.snippet.folder.empty())
                    appendUnique(plan.ignoredFolders, planned.snippet.folder);
                const auto folder = byId(*target.folderId);
                // A vanished folder keeps its id, so the import fails instead of landing in the root.
                planned.folderId = *target.folderId;
                planned.snippet.folder = folder != folders.end() ? folder->name : std::string{};
                return;
            }

            if (planned.snippet.folder.empty())
                return;
            if (const auto folder = byName(planned.snippet.folder); folder != folders.end())
            {
                planned.folderId = folder->id;
                return;
            }
            planned.createsFolder = true;
            appendUnique(plan.newFolders, planned.snippet.folder);
        }

        /**
         * @brief Identifies the target folder, including folders still to create.
         */
        inline std::string folderKey(PlannedSnippet const& planned)
        {
            return planned.createsFolder ? "new:" + planned.snippet.folder : "id:" + planned.folderId;
        }
    }

    /**
     * @brief Plans an import without changing anything.
     *
     * @param pasted The parsed document.
     * @param stored All stored snippets.
     * @param folders All folders, ordered by position; the first of equally named folders wins.
     */
    inline ImportPlan plan(
        std::vector<TransferSnippet> pasted,
        std::vector<Snippet> const& stored,
        std::vector<SnippetFolder> const& folders,
        Target const& target
    )
    {
        ImportPlan result{};
        result.snippets.reserve(pasted.size());
        std::map<std::pair<std::string, std::string>, std::size_t> firstOfName{};

        for (auto& snippet : pasted)
        {
            PlannedSnippet planned{.snippet = std::move(snippet)};
            Detail::resolveFolder(planned, result, folders, target);

            if (!planned.createsFolder)
            {
                // A snippet whose folder no longer exists counts as unfiled, as in the snippets panel.
                const auto sameName = [&planned, &folders](Snippet const& candidate)
                {
                    if (candidate.name != planned.snippet.name)
                        return false;
                    const auto folderExists = std::ranges::any_of(
                        folders,
                        [&candidate](SnippetFolder const& folder)
                        {
                            return folder.id == candidate.folder;
                        }
                    );
                    return (folderExists ? candidate.folder : std::string{}) == planned.folderId;
                };
                const auto identical = std::ranges::find_if(
                    stored,
                    [&](Snippet const& candidate)
                    {
                        return sameName(candidate) && Detail::sameContent(planned.snippet, candidate);
                    }
                );
                const auto conflicting = std::ranges::find_if(stored, sameName);
                if (identical != stored.end())
                {
                    planned.status = Status::Identical;
                    planned.existing = *identical;
                }
                else if (conflicting != stored.end())
                {
                    planned.status = Status::NameConflict;
                    planned.existing = *conflicting;
                }
            }

            const auto key = std::make_pair(Detail::folderKey(planned), planned.snippet.name);
            if (const auto earlier = firstOfName.find(key); earlier != firstOfName.end())
            {
                planned.earlierEntry = earlier->second;
                if (planned.status == Status::New)
                {
                    planned.status = Detail::sameContent(planned.snippet, result.snippets[earlier->second].snippet)
                        ? Status::Identical
                        : Status::NameConflict;
                }
            }
            else
                firstOfName.emplace(key, result.snippets.size());

            result.snippets.push_back(std::move(planned));
        }
        return result;
    }

    /**
     * @brief The entries to store and what they amount to for the user.
     *
     * Favorites are merged quietly: entries that only mark a stored snippet as favorite are part of
     * entries but counted in neither added nor replaced.
     */
    struct Decision
    {
        std::vector<ImportEntry> entries{};
        std::size_t added{0};
        std::size_t replaced{0};
        std::size_t skipped{0};
    };

    /**
     * @brief Turns a plan into import entries.
     *
     * A snippet ends up favorite when any of the identical pasted snippets it absorbs or the stored
     * snippet it matches is one. A skipped conflict leaves the snippet it conflicts with untouched.
     *
     * @param resolutionOf The user's choice for the conflict at the given index of plan.snippets.
     *                     Replace on a conflict that cannot replace counts as Skip.
     */
    inline Decision decide(ImportPlan const& plan, std::function<Resolution(std::size_t)> const& resolutionOf)
    {
        Decision decision{};
        // Index into decision.entries of the entry each planned snippet produced.
        std::vector<std::optional<std::size_t>> entryOf(plan.snippets.size());

        // An identical pasted favorite marks the earlier pasted entry or the stored snippet it matches.
        const auto mergeIdenticalFavorite = [&](std::size_t index)
        {
            auto const& planned = plan.snippets[index];
            if (!planned.snippet.favorite)
                return;
            if (planned.earlierEntry && entryOf[*planned.earlierEntry])
            {
                decision.entries[*entryOf[*planned.earlierEntry]].snippet.favorite = true;
                return;
            }
            if (!planned.existing || planned.existing->favorite)
                return;

            auto const& existing = *planned.existing;
            const auto writtenAlready = std::ranges::find_if(
                decision.entries,
                [&existing](ImportEntry const& entry)
                {
                    return entry.replaces == existing.id;
                }
            );
            if (writtenAlready != decision.entries.end())
            {
                writtenAlready->snippet.favorite = true;
                return;
            }
            entryOf[index] = decision.entries.size();
            decision.entries.push_back(
                ImportEntry{
                    .snippet =
                        TransferSnippet{
                            .name = existing.name,
                            .command = existing.command,
                            .folder = planned.snippet.folder,
                            .tags = existing.tags,
                            .favorite = true,
                            .danger = existing.danger,
                        },
                    .folderId = planned.folderId,
                    .replaces = existing.id,
                }
            );
        };

        for (std::size_t index = 0; index < plan.snippets.size(); ++index)
        {
            auto const& planned = plan.snippets[index];
            auto entry = ImportEntry{.snippet = planned.snippet, .folderId = planned.folderId};

            if (planned.status == Status::Identical)
            {
                ++decision.skipped;
                mergeIdenticalFavorite(index);
                continue;
            }
            if (planned.status == Status::NameConflict)
            {
                const auto resolution = resolutionOf(index);
                if (resolution == Resolution::Skip || (resolution == Resolution::Replace && !planned.canReplace()))
                {
                    ++decision.skipped;
                    continue;
                }
                if (resolution == Resolution::Replace)
                {
                    entry.replaces = planned.existing->id;
                    entry.snippet.favorite = entry.snippet.favorite || planned.existing->favorite;
                }
            }
            ++(entry.replaces.empty() ? decision.added : decision.replaced);
            entryOf[index] = decision.entries.size();
            decision.entries.push_back(std::move(entry));
        }
        return decision;
    }
}
