#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace SharedData
{
    /** @brief A single .gitignore-style pattern, pre-parsed for fast matching. */
    struct IgnorePattern
    {
        std::string pattern;
        bool negated{false};
        bool directoryOnly{false};
        bool anchored{false};
    };

    /** @brief Rules parsed from one ignore file, tagged with the directory they apply under. */
    struct IgnoreRuleSet
    {
        std::filesystem::path originDir{};
        std::vector<IgnorePattern> patterns{};
    };

    /** @brief Glob match against a single path segment / full subpath.
     *
     * Supports `*` (matches any chars except '/'), `**` (matches across '/'), and `?` (one char).
     * Character classes (`[abc]`) are not supported.
     */
    bool globMatch(std::string_view pattern, std::string_view text);

    /** @brief Parses an ignore file's contents into a list of IgnorePattern objects.
     *         Blank lines and lines beginning with '#' are skipped.
     */
    std::vector<IgnorePattern> parseIgnoreFile(std::string_view content);

    /** @brief Stateful accumulator for .gitignore/.ignore rules encountered during a scan.
     *
     * Usage: during directory traversal, call `addFile(relDir, content)` for each ignore file
     * found; call `isIgnored(relPath, isDirectory)` to test entries.  All paths are expressed
     * relative to the scan root.
     */
    class IgnoreMatcher
    {
      public:
        /** @brief Register parsed patterns from one ignore file living at @p relDir. */
        void addFile(std::filesystem::path const& relDir, std::string_view content);

        /** @brief True if @p relPath is ignored under any currently-known rule set.
         *
         * @param relPath    Entry path relative to the scan root (use '/' separators).
         * @param isDirectory Whether the entry is a directory (relevant for directoryOnly rules).
         */
        bool isIgnored(std::filesystem::path const& relPath, bool isDirectory) const;

        bool empty() const noexcept
        {
            return ruleSets_.empty();
        }

      private:
        std::vector<IgnoreRuleSet> ruleSets_{};
    };
}
