#pragma once

#include <libssh/libssh.h>

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace SecureShell
{
    /**
     * @brief Why a host key did not pass the known hosts files.
     */
    enum class HostKeyProblem
    {
        /** @brief No known hosts file holds a key for the server. */
        Unknown,
        /** @brief They hold another key of the same type: the server has a new key, or someone intercepts. */
        Changed,
        /** @brief They hold only keys of other types for the server. */
        OtherType,
        /**
         * @brief No known hosts file holds a key for the server, and it is another key than the one that was trusted
         *        for the server before, which the known hosts file did not take or no longer holds. Never set by
         *        verifyHostKey(), which knows of no such trust.
         */
        ChangedSinceTrusted,
    };

    /**
     * @brief A host key that did not pass the known hosts files. What a host key prompt shows.
     */
    struct HostKeyQuestion
    {
        HostKeyProblem problem{HostKeyProblem::Unknown};
        /** @brief Whether the key is the one of a ProxyJump host on the way to the server, not of the server. */
        bool jumpHost{false};
        /** @brief The server as the known hosts files name it, after the ssh config resolved the host. */
        std::string host{};
        int port{};
        /** @brief The type of the key as a known hosts file writes it, "ssh-ed25519" for one. */
        std::string keyType{};
        /** @brief "SHA256:" and the hash of the key in base64, as OpenSSH prints it. */
        std::string fingerprint{};
        /**
         * @brief The known hosts file of the user, empty when the session has none. Of a key that is Changed or
         *        OtherType the known hosts file that holds the other key, which may be the global one.
         */
        std::string knownHostsFile{};
        /** @brief The key trusted before, type and fingerprint, of a key that is ChangedSinceTrusted. */
        std::string trustedKey{};
        /**
         * @brief The command that removes the keys of the server from knownHostsFile, as OpenSSH names it when it
         *        refuses a changed key. Of a key that is Changed or OtherType, and on Linux only, else empty.
         */
        std::string removalCommand{};
    };

    /**
     * @brief What the user decided about a host key. It names no key: the key the server presented is the one trusted.
     */
    enum class HostKeyAnswer
    {
        /** @brief Also a prompt that was cancelled or could not be shown. */
        Refused,
        /**
         * @brief For this connection, the known hosts file stays as it is. Nothing a user is offered: for a key
         *        that was trusted before and that the known hosts file does not hold.
         */
        Once,
        /** @brief Added to the known hosts file. */
        Always,
    };

    /**
     * @brief Asks whether to trust an unknown host key. A changed key or one of another type is put to it too, so the
     *        user is warned: it is refused whatever the answer.
     */
    using HostKeySource = std::function<HostKeyAnswer(HostKeyQuestion const& question)>;

    /**
     * @brief Checks the host key of the connected @p session against the known hosts files, before anything is sent
     *        that relies on who the server is. A key the files hold for the server passes. A key that differs from the
     *        one they hold, or is of another type, is refused after @p askAboutHostKey heard of it. An unknown key is
     *        refused when @p strict, else @p askAboutHostKey decides, and Always adds the key to the known hosts file
     *        of the user. "StrictHostKeyChecking no" of an ssh config changes none of this.
     *
     * @param askAboutHostKey Empty refuses every unknown key and warns of no changed one.
     * @return Why the key was refused.
     */
    std::expected<void, std::string>
    verifyHostKey(ssh_session session, bool strict, HostKeySource const& askAboutHostKey);

    /**
     * @brief A host key of a ProxyJump host that verifyJumpHostKey() refused.
     */
    struct JumpHostRefusal
    {
        HostKeyQuestion question{};
        /** @brief Why the key was refused, and what to do about it. */
        std::string reason{};
    };

    /**
     * @brief The verification of the ProxyJump hosts of one connect. libssh connects to a jump host in a thread of
     *        its own, which takes the verification that is going on when it starts, checks the host key with
     *        verifyJumpHostKey() in place of the check of libssh, which "StrictHostKeyChecking no" of an ssh config
     *        turns off, and leaves a refusal with it. A jump host that starts when none is going on is refused.
     *        Connects are expected to use jump hosts one at a time.
     */
    class JumpHostVerification
    {
      public:
        /**
         * @brief Has libssh verify every ProxyJump host of @p session this way. Before the connect.
         *
         * @param knownHostsFile The known hosts file the session set of its own, which its jump hosts take too:
         *        libssh makes their sessions without it. A key trusted always on a direct connect to a jump host
         *        is then the one the jump host is looked up by. Empty leaves each jump host the file libssh and
         *        the ssh config give it.
         * @return Nothing when libssh did not take it.
         */
        static std::optional<JumpHostVerification> begin(ssh_session session, std::string knownHostsFile);

        /**
         * @brief Ends the verification: jump hosts that start from here on are refused. What a jump host that took
         *        it before refuses reaches nobody.
         */
        ~JumpHostVerification();
        JumpHostVerification(JumpHostVerification const&) = delete;
        JumpHostVerification& operator=(JumpHostVerification const&) = delete;
        JumpHostVerification(JumpHostVerification&&) noexcept;
        JumpHostVerification& operator=(JumpHostVerification&&) noexcept;

        /**
         * @brief The jump host key that was refused last in this connect, which it forgets. Thread-safe.
         */
        std::optional<JumpHostRefusal> takeRefusal();

        struct Record;

      private:
        explicit JumpHostVerification(std::shared_ptr<Record> record);

        std::shared_ptr<Record> record_;
    };

    /**
     * @brief Checks the host key of the connected ProxyJump host of @p jumpSession against its known hosts files.
     *        Only a key they hold for the jump host passes. Nobody is asked about another one: libssh connects to a
     *        jump host in a thread of its own, while the connect to the server waits with its timeout running.
     *
     * @return The refused key and why it was refused.
     */
    std::expected<void, JumpHostRefusal> verifyJumpHostKey(ssh_session jumpSession);
}
