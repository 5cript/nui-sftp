#pragma once

#include <ftp/support/ftp_script.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace FileTransferProtocol::Test
{
    /**
     * @brief One entry of a ServedTree. Names are bytes as they are listed, so a tree may hold names no file system
     *        has.
     */
    struct ServedEntry
    {
        enum class Type
        {
            File,
            Directory,
            Link,
        };

        std::string name{};
        Type type{Type::File};
        std::string content{};
        /** @brief Where a link leads, absolute or from the directory that holds it. */
        std::string target{};
        std::vector<ServedEntry> children{};
    };

    ServedEntry servedFile(std::string name, std::string content);
    ServedEntry servedDirectory(std::string name, std::vector<ServedEntry> children);
    ServedEntry servedLink(std::string name, std::string target);

    /**
     * @brief A directory tree that TreeResponders serve, shared by the control connections of a server. A path is
     *        looked up by its segments like a file system does: links lead on, "." and ".." are resolved, the first
     *        entry of a name counts. Thread-safe.
     */
    class ServedTree
    {
      public:
        enum class Removal
        {
            Removed,
            Missing,
            WrongType,
            NotEmpty,
        };

        /**
         * @param root The entries of "/".
         */
        explicit ServedTree(std::vector<ServedEntry> root);

        /**
         * @brief The path of the directory @p path leads to, its links resolved, nothing for no directory.
         */
        std::optional<std::string> directoryAt(std::string_view path);

        /**
         * @brief The lines LIST gives for the directory @p path, nothing for no directory.
         */
        std::optional<std::string> listingOf(std::string_view path);

        /**
         * @brief The content of the file @p path leads to, nothing for no file.
         */
        std::optional<std::string> contentOf(std::string_view path);

        /**
         * @brief contentOf() for a RETR, which is recorded as the path of the file it leads to.
         */
        std::optional<std::string> retrieve(std::string_view path);

        /**
         * @brief Removes the file or link @p path is, its last segment is not followed.
         */
        Removal removeFile(std::string_view path);

        /**
         * @brief Removes the empty directory @p path is, its last segment is not followed.
         */
        Removal removeDirectory(std::string_view path);

        /**
         * @brief Whether @p path leads to an entry, its last segment not followed.
         */
        bool exists(std::string_view path);

        /** @brief The directories listed so far, as directoryAt() gives them. */
        std::vector<std::string> listed() const;

        /** @brief The paths removed so far, as they were asked for. */
        std::vector<std::string> removed() const;

        /** @brief The files retrieved so far, their links resolved. */
        std::vector<std::string> retrieved() const;

      private:
        Removal remove(std::string_view path, ServedEntry::Type type);

        mutable std::mutex mutex_{};
        ServedEntry root_;
        std::vector<std::string> listed_{};
        std::vector<std::string> removed_{};
        std::vector<std::string> retrieved_{};
    };

    /**
     * @brief Serves a ServedTree on one control connection like a read-only Unix server that may delete: a plain
     *        login, LIST in the format of vsftpd, RETR with REST, SIZE, MDTM, DELE and RMD. STAT and MLSD are
     *        unknown, every command that writes is denied.
     */
    class TreeResponder : public Responder
    {
      public:
        explicit TreeResponder(std::shared_ptr<ServedTree> tree);
        ~TreeResponder() override = default;
        TreeResponder(TreeResponder const&) = delete;
        TreeResponder& operator=(TreeResponder const&) = delete;
        TreeResponder(TreeResponder&&) = delete;
        TreeResponder& operator=(TreeResponder&&) = delete;

        Script answer(std::string_view line) override;

      private:
        std::string absolute(std::string_view path) const;

        std::shared_ptr<ServedTree> tree_;
        std::string workingDirectory_{"/"};
        std::uint64_t restartAt_{0u};
        bool passiveOpen_{false};
    };

    /**
     * @brief A script that greets and serves @p tree.
     */
    Script serving(std::shared_ptr<ServedTree> tree);
}
