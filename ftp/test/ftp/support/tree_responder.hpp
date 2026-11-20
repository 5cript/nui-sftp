#pragma once

#include <ftp/support/ftp_script.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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
        /** @brief The permissions LIST shows for a file. */
        std::string permissions{"rw-r--r--"};
        /** @brief Where a link leads, absolute or from the directory that holds it. */
        std::string target{};
        std::vector<ServedEntry> children{};
        /** @brief The time MFMT set, as YYYYMMDDHHMMSS in UTC. Unset, every entry shows the same time. */
        std::optional<std::string> modified{};
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
         * @brief Where an upload writes in its file.
         */
        enum class Upload
        {
            /** @brief At its offset, keeping what lies behind it, at the start of an emptied file without one. */
            Store,
            /** @brief At the end. */
            Append,
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

        /**
         * @brief Opens the file @p path leads to for an upload like STOR or APPE: a missing file is created in an
         *        existing directory.
         *
         * @param offset Where a Store writes, from a REST.
         * @return The position the upload writes at, nothing for a directory or a missing directory.
         */
        std::optional<std::uint64_t> openForUpload(std::string_view path, Upload upload, std::uint64_t offset);

        /**
         * @brief Writes @p bytes at @p position of the file @p path leads to, a gap before it is filled with zeros.
         *
         * @return False where no file is.
         */
        bool write(std::string_view path, std::uint64_t position, std::string_view bytes);

        /**
         * @brief Puts a file with @p content at @p path, replacing a file there. For the setup of a test.
         *
         * @return False for a directory or a missing directory.
         */
        bool putFile(std::string_view path, std::string content);

        /**
         * @brief Creates the directory @p path in an existing directory.
         *
         * @return False where @p path exists or its directory does not.
         */
        bool makeDirectory(std::string_view path);

        /**
         * @brief Moves @p from to @p to like rename(2): neither last segment is followed, a file replaces a file and
         *        a directory an empty directory.
         *
         * @return False where @p from or the directory of @p to is missing, the types do not fit or a directory
         *         would move into itself.
         */
        bool rename(std::string_view from, std::string_view to);

        /**
         * @brief Sets the time of the entry @p path leads to, @p time as YYYYMMDDHHMMSS.
         *
         * @return False where nothing is.
         */
        bool setModified(std::string_view path, std::string time);

        /**
         * @brief The time MDTM tells of the file @p path leads to, nothing for no file.
         */
        std::optional<std::string> modifiedOf(std::string_view path);

        /** @brief The directories listed so far, as directoryAt() gives them. */
        std::vector<std::string> listed() const;

        /** @brief The paths removed so far, as they were asked for. */
        std::vector<std::string> removed() const;

        /** @brief The files retrieved so far, their links resolved. */
        std::vector<std::string> retrieved() const;

        /** @brief The files opened for uploads so far, their links resolved. */
        std::vector<std::string> stored() const;

      private:
        Removal remove(std::string_view path, ServedEntry::Type type);

        /**
         * @brief The file @p path leads to, created in an existing directory where none is. Needs the lock.
         *
         * @return The file and its path, nothing for a directory or a missing directory.
         */
        std::optional<std::pair<ServedEntry*, std::string>> fileAt(std::string_view path);

        mutable std::mutex mutex_{};
        ServedEntry root_;
        std::vector<std::string> listed_{};
        std::vector<std::string> removed_{};
        std::vector<std::string> retrieved_{};
        std::vector<std::string> stored_{};
    };

    /**
     * @brief What a TreeResponder's FEAT lists and serves beyond the commands every server has.
     */
    struct ServedFeatures
    {
        /**
         * @brief SIZE is listed and answered. Without it and without MLST, which is never served, a client cannot
         *        look up a single path.
         */
        bool size{true};
    };

    /**
     * @brief Serves a ServedTree on one control connection like a Unix server: a plain login, LIST in the format of
     *        vsftpd, RETR, STOR and APPE with REST, SIZE, MDTM, MFMT, MKD, RNFR and RNTO, DELE and RMD. STAT, MLSD
     *        and MLST are unknown, SITE is denied.
     */
    class TreeResponder : public Responder
    {
      public:
        explicit TreeResponder(std::shared_ptr<ServedTree> tree, ServedFeatures features = {});
        ~TreeResponder() override = default;
        TreeResponder(TreeResponder const&) = delete;
        TreeResponder& operator=(TreeResponder const&) = delete;
        TreeResponder(TreeResponder&&) = delete;
        TreeResponder& operator=(TreeResponder&&) = delete;

        Script answer(std::string_view line) override;

      private:
        std::string absolute(std::string_view path) const;
        Script upload(std::string_view verb, std::string_view argument);
        Script renameTo(std::optional<std::string> from, std::string_view argument);
        Script setModified(std::string_view argument);

        std::shared_ptr<ServedTree> tree_;
        ServedFeatures features_;
        std::string workingDirectory_{"/"};
        std::uint64_t restartAt_{0u};
        bool passiveOpen_{false};
        /** @brief The path of an RNFR, for the RNTO right behind it. */
        std::optional<std::string> renameFrom_{};
    };

    /**
     * @brief A script that greets and serves @p tree.
     */
    Script serving(std::shared_ptr<ServedTree> tree, ServedFeatures features = {});
}
