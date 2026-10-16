#pragma once

#include <fmt/format.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace SessionInternal
{
    enum class WriteOutcome
    {
        Written,
        /**
         * @brief The file existed and was left alone.
         */
        Exists,
        Failed,
    };

    /**
     * @brief "{stem}_{index}{extension}".
     */
    inline std::string numberedFileName(std::string_view stem, int index, std::string_view extension)
    {
        return fmt::format("{}_{}{}", stem, index, extension);
    }

    /**
     * @brief Plans saving several contents to numbered files that do not exist yet: content after content, each to
     *        the next free name "{stem}_{index}{extension}". A name that exists moves the content on to the next number,
     *        at most @p attemptsPerName names per content.
     */
    class NumberedSaves
    {
      public:
        /**
         * @brief The content to write next and the name to try.
         */
        struct Attempt
        {
            std::size_t content;
            std::string name;
        };

        /**
         * @brief A content that was not saved.
         */
        struct Failure
        {
            std::size_t content;
            /**
             * @brief The last name tried.
             */
            std::string name;
            /**
             * @brief True when every tried name existed, false when writing failed.
             */
            bool noFreeName;
        };

        NumberedSaves(std::string stem, std::string extension, std::size_t contentCount, int attemptsPerName = 1000)
            : stem_{std::move(stem)}
            , extension_{std::move(extension)}
            , contentCount_{contentCount}
            , attemptsPerName_{attemptsPerName}
        {}

        /**
         * @brief What to write next, nothing when every content is saved or failed.
         */
        std::optional<Attempt> next() const
        {
            if (content_ >= contentCount_)
                return std::nullopt;
            return Attempt{.content = content_, .name = currentName()};
        }

        /**
         * @brief Takes the outcome of writing the attempt @ref next gave.
         */
        void record(WriteOutcome outcome)
        {
            if (content_ >= contentCount_)
                return;
            switch (outcome)
            {
                case WriteOutcome::Written:
                    written_.push_back(currentName());
                    ++index_;
                    return finishContent();
                case WriteOutcome::Exists:
                    ++index_;
                    if (++attempts_ < attemptsPerName_)
                        return;
                    failures_.push_back(
                        Failure{.content = content_, .name = numberedFileName(stem_, index_ - 1, extension_), .noFreeName = true}
                    );
                    return finishContent();
                case WriteOutcome::Failed:
                    failures_.push_back(Failure{.content = content_, .name = currentName(), .noFreeName = false});
                    ++index_;
                    return finishContent();
            }
        }

        /**
         * @brief The names written, in content order.
         */
        std::vector<std::string> const& written() const
        {
            return written_;
        }

        std::vector<Failure> const& failures() const
        {
            return failures_;
        }

      private:
        std::string currentName() const
        {
            return numberedFileName(stem_, index_, extension_);
        }

        void finishContent()
        {
            ++content_;
            attempts_ = 0;
        }

        std::string stem_;
        std::string extension_;
        std::size_t contentCount_;
        int attemptsPerName_;
        std::size_t content_{0};
        int index_{0};
        int attempts_{0};
        std::vector<std::string> written_{};
        std::vector<Failure> failures_{};
    };
}
