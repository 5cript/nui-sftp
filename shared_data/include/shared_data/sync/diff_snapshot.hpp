#pragma once

#include <shared_data/shared_data.hpp>
#include <shared_data/sync/diff_summary.hpp>
#include <shared_data/sync/diff_tree_node.hpp>

#include <utility/describe.hpp>

#include <string>
#include <vector>

namespace SharedData::Sync
{
    /**
     * @brief Rows below one parent of a section. An empty parent is the section root.
     */
    struct SectionChildren
    {
        std::string parentRelKey{};
        std::vector<DiffTreeNode> children{};

        bool operator==(SectionChildren const&) const = default;
    };
    BOOST_DESCRIBE_STRUCT(SectionChildren, (), (parentRelKey, children))

    /**
     * @brief Directories whose children the frontend has loaded, per section. A recompute sends them back
     *        so expanded directories stay expanded.
     */
    struct LoadedParents
    {
        std::vector<std::string> uploads{};
        std::vector<std::string> downloads{};
        std::vector<std::string> deletes{};
    };
    BOOST_DESCRIBE_STRUCT(LoadedParents, (), (uploads, downloads, deletes))

    /**
     * @brief Everything the dialog needs after a recompute: the summary, the roots and the children of
     *        every requested parent that still exists, parents before their descendants.
     */
    struct DiffSnapshot
    {
        DiffSummary summary{};
        std::vector<SectionChildren> uploads{};
        std::vector<SectionChildren> downloads{};
        std::vector<SectionChildren> deletes{};
    };
    BOOST_DESCRIBE_STRUCT(DiffSnapshot, (), (summary, uploads, downloads, deletes))

    inline void to_json(nlohmann::json& j, SectionChildren const& s)
    {
        SharedData::to_json(j, s);
    }
    inline void from_json(nlohmann::json const& j, SectionChildren& s)
    {
        SharedData::from_json(j, s);
    }
    inline void to_json(nlohmann::json& j, LoadedParents const& s)
    {
        SharedData::to_json(j, s);
    }
    inline void from_json(nlohmann::json const& j, LoadedParents& s)
    {
        SharedData::from_json(j, s);
    }
    inline void to_json(nlohmann::json& j, DiffSnapshot const& s)
    {
        SharedData::to_json(j, s);
    }
    inline void from_json(nlohmann::json const& j, DiffSnapshot& s)
    {
        SharedData::from_json(j, s);
    }
}
