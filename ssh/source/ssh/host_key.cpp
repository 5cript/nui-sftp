#include <ssh/host_key.hpp>
#include <ssh/ssh_key.hpp>
#include <log/log.hpp>
#include <paths/native.hpp>
#include <utility/algorithm/case_convert.hpp>

#include <fmt/format.h>
#include <libssh/callbacks.h>

#include <cstddef>
#include <exception>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

namespace SecureShell
{
    namespace
    {
        struct StringDeleter
        {
            void operator()(char* text) const noexcept
            {
                ssh_string_free_char(text);
            }
        };
        using OwnedString = std::unique_ptr<char, StringDeleter>;

        struct HashDeleter
        {
            void operator()(unsigned char* hash) const noexcept
            {
                ssh_clean_pubkey_hash(&hash);
            }
        };
        using OwnedHash = std::unique_ptr<unsigned char, HashDeleter>;

        /**
         * @return Nothing when the session has no such option.
         */
        OwnedString optionOf(ssh_session session, ssh_options_e option)
        {
            char* value = nullptr;
            if (ssh_options_get(session, option, &value) != SSH_OK)
                return nullptr;
            return OwnedString{value};
        }

        /**
         * @brief The key @p session presented, and the server as the known hosts files name it.
         */
        std::expected<HostKeyQuestion, std::string> presentedKeyOf(ssh_session session)
        {
            SshKey key{};
            if (ssh_get_server_publickey(session, &key.underlyingReferenced()) != SSH_OK || !key)
                return std::unexpected{std::string{"The server presented no host key."}};

            unsigned char* rawHash = nullptr;
            std::size_t hashLength = 0u;
            if (ssh_get_publickey_hash(key.underlying(), SSH_PUBLICKEY_HASH_SHA256, &rawHash, &hashLength) != 0)
                return std::unexpected{std::string{"The host key of the server could not be hashed."}};
            const OwnedHash hash{rawHash};

            const OwnedString fingerprint{
                ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash.get(), hashLength)
            };
            const auto host = optionOf(session, SSH_OPTIONS_HOST);
            unsigned int port = 0u;
            const auto* const keyType = ssh_key_type_to_char(ssh_key_type(key.underlying()));
            if (!fingerprint || !host || !keyType || ssh_options_get_port(session, &port) != SSH_OK)
                return std::unexpected{std::string{"The host key of the server could not be described."}};

            const auto knownHostsFile = optionOf(session, SSH_OPTIONS_KNOWNHOSTS);
            return HostKeyQuestion{
                .host = host.get(),
                .port = static_cast<int>(port),
                .keyType = keyType,
                .fingerprint = fingerprint.get(),
                .knownHostsFile = knownHostsFile ? knownHostsFile.get() : "",
            };
        }

        std::string serverOf(HostKeyQuestion const& presented)
        {
            return fmt::format("{}:{}", presented.host, presented.port);
        }

        std::string keyOf(HostKeyQuestion const& presented)
        {
            return fmt::format("{} {}", presented.keyType, presented.fingerprint);
        }

        std::string knownHostsFileOf(HostKeyQuestion const& presented)
        {
            if (presented.knownHostsFile.empty())
                return "the known hosts file";
            return fmt::format("'{}'", presented.knownHostsFile);
        }

        /**
         * @brief What the texts about a host key call its server.
         */
        struct ServerWording
        {
            /** @brief Before the server in "the host key of". */
            std::string_view prefix;
            std::string_view noun;
            std::string_view capitalizedNoun;
        };
        constexpr ServerWording serverWording{.prefix = "", .noun = "the server", .capitalizedNoun = "The server"};
        constexpr ServerWording jumpHostWording{
            .prefix = "the jump host ",
            .noun = "the jump host",
            .capitalizedNoun = "The jump host",
        };

        /**
         * @brief Why a key is refused that the known hosts files hold another one for. Logged too.
         *
         * @param presented The key, its problem is Changed or OtherType.
         */
        std::string changedKeyRefusal(HostKeyQuestion const& presented, ServerWording const& wording)
        {
            Log::warn(
                "The host key of {}{} is not the one the known hosts files hold for it, the connection is refused: {}",
                wording.prefix,
                serverOf(presented),
                keyOf(presented)
            );
            return fmt::format(
                "The host key of {}{} is not the one the known hosts files hold for it{}. {} may have a new key, or "
                "someone may be intercepting the connection.\nPresented key: {}\nRemove the old key from {} only when "
                "the administrator of {} confirms the new one.",
                wording.prefix,
                serverOf(presented),
                presented.problem == HostKeyProblem::OtherType ? ", they hold a key of another type" : "",
                wording.capitalizedNoun,
                keyOf(presented),
                knownHostsFileOf(presented),
                wording.noun
            );
        }

        /**
         * @brief Refuses @p changed, a key the known hosts files hold another one for, after @p askAboutHostKey
         *        showed the warning. Its answer changes nothing.
         */
        std::unexpected<std::string> refuseChangedKey(HostKeyQuestion const& changed, HostKeySource const& askAboutHostKey)
        {
            auto refusal = changedKeyRefusal(changed, serverWording);
            if (askAboutHostKey)
                askAboutHostKey(changed);
            return std::unexpected{std::move(refusal)};
        }

        struct EntryDeleter
        {
            void operator()(ssh_knownhosts_entry* entry) const noexcept
            {
                ssh_knownhosts_entry_free(entry);
            }
        };
        using OwnedEntry = std::unique_ptr<ssh_knownhosts_entry, EntryDeleter>;

        /**
         * @brief The server of @p presented as a known hosts file names it, which is how libssh looks it up.
         */
        std::string knownHostsNameOf(HostKeyQuestion const& presented)
        {
            const auto host = Utility::Algorithm::toLowerCase(presented.host);
            if (presented.port == 22)
                return host;
            return fmt::format("[{}]:{}", host, presented.port);
        }

        /**
         * @return Nothing for a line that holds no key of @p server or one libssh cannot read: a comment, a marker
         *         as libssh leaves them out itself, another server, a key of a type libssh does not know.
         */
        OwnedEntry entryOf(std::string const& line, std::string const& server)
        {
            const auto first = line.find_first_not_of(" \t\r");
            if (first == std::string::npos || line[first] == '#' || line[first] == '@')
                return nullptr;
            ssh_knownhosts_entry* entry = nullptr;
            if (ssh_known_hosts_parse_line(server.c_str(), line.c_str(), &entry) != SSH_OK)
                return nullptr;
            return OwnedEntry{entry};
        }

        /**
         * @brief Compares @p key with the keys @p file holds for @p server, as libssh does, but leaves out the lines
         *        libssh cannot read.
         */
        ssh_known_hosts_e lookUpInFile(std::string const& file, std::string const& server, ssh_key key)
        {
            auto found = SSH_KNOWN_HOSTS_UNKNOWN;
            if (file.empty())
                return found;
            std::ifstream stream{Paths::nativeFromUtf8(file)};
            for (std::string line; std::getline(stream, line);)
            {
                const auto entry = entryOf(line, server);
                if (!entry || !entry->publickey)
                    continue;
                if (ssh_key_cmp(key, entry->publickey, SSH_KEY_CMP_PUBLIC) == 0)
                    return SSH_KNOWN_HOSTS_OK;
                if (ssh_key_type(key) == ssh_key_type(entry->publickey))
                    found = SSH_KNOWN_HOSTS_CHANGED;
                else if (found != SSH_KNOWN_HOSTS_CHANGED)
                    found = SSH_KNOWN_HOSTS_OTHER;
            }
            return found;
        }

        /**
         * @brief Looks the key of @p session up in its known hosts files line by line. libssh gives up on all lines
         *        of a server when one of them holds a key of a type it does not know, as OpenSSH 10 writes them, and
         *        calls the server unknown then, also when the key changed.
         */
        ssh_known_hosts_e lookUpLineByLine(ssh_session session, HostKeyQuestion const& presented)
        {
            SshKey key{};
            if (ssh_get_server_publickey(session, &key.underlyingReferenced()) != SSH_OK || !key)
                return SSH_KNOWN_HOSTS_ERROR;

            const auto server = knownHostsNameOf(presented);
            const auto ofUser = lookUpInFile(presented.knownHostsFile, server, key.underlying());
            if (ofUser == SSH_KNOWN_HOSTS_OK)
                return ofUser;
            const auto globalFile = optionOf(session, SSH_OPTIONS_GLOBAL_KNOWNHOSTS);
            const auto global = lookUpInFile(globalFile ? globalFile.get() : "", server, key.underlying());
            // As libssh puts the two files together.
            return global == SSH_KNOWN_HOSTS_UNKNOWN ? ofUser : global;
        }

        bool holdsAnotherKey(ssh_known_hosts_e found)
        {
            return found == SSH_KNOWN_HOSTS_CHANGED || found == SSH_KNOWN_HOSTS_OTHER;
        }

        /**
         * @brief The known hosts file that holds another key for the server of @p session than the presented one,
         *        the file of the user before the global one.
         */
        std::string fileHoldingAnotherKey(ssh_session session, HostKeyQuestion const& presented)
        {
            SshKey key{};
            if (ssh_get_server_publickey(session, &key.underlyingReferenced()) != SSH_OK || !key)
                return presented.knownHostsFile;

            const auto server = knownHostsNameOf(presented);
            if (holdsAnotherKey(lookUpInFile(presented.knownHostsFile, server, key.underlying())))
                return presented.knownHostsFile;
            const auto globalFile = optionOf(session, SSH_OPTIONS_GLOBAL_KNOWNHOSTS);
            if (globalFile && holdsAnotherKey(lookUpInFile(globalFile.get(), server, key.underlying())))
                return globalFile.get();
            return presented.knownHostsFile;
        }

        /**
         * @brief @p text as one word of a POSIX shell.
         */
        [[maybe_unused]] std::string shellQuoted(std::string_view text)
        {
            std::string quoted{"'"};
            for (const auto character : text)
            {
                if (character == '\'')
                    quoted += "'\\''";
                else
                    quoted += character;
            }
            quoted += '\'';
            return quoted;
        }

        /**
         * @brief The command that removes the keys of the server of @p changed from its known hosts file.
         *
         * @return Nothing on other platforms than Linux, or without a known hosts file.
         */
        std::string removalCommandOf([[maybe_unused]] HostKeyQuestion const& changed)
        {
#ifdef __linux__
            if (changed.knownHostsFile.empty())
                return {};
            return fmt::format(
                "ssh-keygen -f {} -R {}", shellQuoted(changed.knownHostsFile), shellQuoted(knownHostsNameOf(changed))
            );
#else
            return {};
#endif
        }

        /**
         * @brief @p presented as the key the known hosts files hold another one for, with the file that does.
         *
         * @param found SSH_KNOWN_HOSTS_CHANGED or SSH_KNOWN_HOSTS_OTHER.
         */
        HostKeyQuestion changedKeyOf(ssh_session session, HostKeyQuestion presented, ssh_known_hosts_e found)
        {
            presented.problem =
                found == SSH_KNOWN_HOSTS_OTHER ? HostKeyProblem::OtherType : HostKeyProblem::Changed;
            presented.knownHostsFile = fileHoldingAnotherKey(session, presented);
            presented.removalCommand = removalCommandOf(presented);
            return presented;
        }

        /**
         * @brief Looks the key of @p session up in the known hosts files with the StrictHostKeyChecking of libssh on.
         *        "StrictHostKeyChecking no" in an ssh config turns it off, and then the lookup calls every key known
         *        that the files do not hold, a changed one too.
         */
        ssh_known_hosts_e lookUpStrictly(ssh_session session, HostKeyQuestion const& presented)
        {
            const int strictCheckingOfLibssh = 1;
            if (ssh_options_set(session, SSH_OPTIONS_STRICTHOSTKEYCHECK, &strictCheckingOfLibssh) != SSH_OK)
                return SSH_KNOWN_HOSTS_ERROR;
            const auto known = ssh_session_is_known_server(session);
            if (known != SSH_KNOWN_HOSTS_UNKNOWN && known != SSH_KNOWN_HOSTS_NOT_FOUND)
                return known;
            return lookUpLineByLine(session, presented);
        }

        std::string uncheckedKey(HostKeyQuestion const& presented, ServerWording const& wording, ssh_session session)
        {
            return fmt::format(
                "The host key of {}{} could not be checked: {}",
                wording.prefix,
                serverOf(presented),
                ssh_get_error(session)
            );
        }

        std::string unknownJumpHostKeyRefusal(HostKeyQuestion const& presented)
        {
            Log::warn(
                "The host key of the jump host {} is in no known hosts file, the connection is refused: {}",
                serverOf(presented),
                keyOf(presented)
            );
            return fmt::format(
                "The host key of the jump host {} is in no known hosts file, and jump hosts are not asked about."
                "\nPresented key: {}\nConnect to the jump host directly and trust its key always, or add the key to {}.",
                serverOf(presented),
                keyOf(presented),
                knownHostsFileOf(presented)
            );
        }

        std::unexpected<JumpHostRefusal> refusedJumpHost(HostKeyQuestion question, std::string reason)
        {
            return std::unexpected{JumpHostRefusal{.question = std::move(question), .reason = std::move(reason)}};
        }
    }

    struct JumpHostVerification::Record
    {
        /** @brief Set before a jump host can take the record, and left alone from then on. */
        std::string knownHostsFile{};
        std::mutex mutex{};
        std::optional<JumpHostRefusal> refusal{};
    };

    namespace
    {
        /**
         * @brief The verification that is going on. Never destroyed: a jump thread of libssh may outlive the statics
         *        of the program.
         */
        struct VerificationGoingOn
        {
            std::mutex mutex{};
            std::shared_ptr<JumpHostVerification::Record> record{};
        };

        VerificationGoingOn& verificationGoingOn()
        {
            static auto* const goingOn = new VerificationGoingOn{};
            return *goingOn;
        }

        /**
         * @brief The verification a jump thread of libssh took when it started, kept for as long as the thread runs.
         */
        thread_local std::shared_ptr<JumpHostVerification::Record> verificationOfThisJumpThread{};

        /**
         * @brief Runs @p function in a jump thread of libssh, which nothing may be thrown into.
         *
         * @return SSH_ERROR for what it throws.
         */
        template <typename Function>
        int inJumpThread(Function const& function) noexcept
        {
            try
            {
                return function();
            }
            catch (std::exception const& exception)
            {
                Log::error("Verifying the host key of a jump host failed, it is refused: {}", exception.what());
            }
            catch (...)
            {
                Log::error("Verifying the host key of a jump host failed, it is refused.");
            }
            return SSH_ERROR;
        }

        ssh_jump_callbacks_struct& jumpHostCallbacks();

        /**
         * @brief Before libssh connects to a jump host: takes the verification that is going on, hands the jump host
         *        its known hosts file and has the jump hosts behind it verified the same way, which libssh takes
         *        from the session of the jump host before them.
         */
        int beginJumpHost(ssh_session jumpSession, void*)
        {
            return inJumpThread(
                [jumpSession]
                {
                    {
                        auto& goingOn = verificationGoingOn();
                        std::scoped_lock lock{goingOn.mutex};
                        verificationOfThisJumpThread = goingOn.record;
                    }
                    auto const& verification = verificationOfThisJumpThread;
                    // Without a word in the log: the thread may be all that is left of a program that is ending.
                    if (!verification)
                        return SSH_ERROR;
                    auto const& knownHostsFile = verification->knownHostsFile;
                    if (!knownHostsFile.empty() &&
                        ssh_options_set(jumpSession, SSH_OPTIONS_KNOWNHOSTS, knownHostsFile.c_str()) != SSH_OK)
                        return SSH_ERROR;
                    return ssh_options_set(jumpSession, SSH_OPTIONS_PROXYJUMP_CB_LIST_APPEND, &jumpHostCallbacks());
                }
            );
        }

        /**
         * @brief In place of the check of libssh. The refusal is left with the verification the thread took.
         */
        int verifyJumpHost(ssh_session jumpSession, void*)
        {
            return inJumpThread(
                [jumpSession]
                {
                    auto verified = verifyJumpHostKey(jumpSession);
                    if (verified)
                        return SSH_OK;
                    if (auto const& verification = verificationOfThisJumpThread; verification)
                    {
                        std::scoped_lock lock{verification->mutex};
                        verification->refusal = std::move(verified).error();
                    }
                    return SSH_ERROR;
                }
            );
        }

        /**
         * @brief Never destroyed either, libssh keeps the pointer for its jump threads.
         */
        ssh_jump_callbacks_struct& jumpHostCallbacks()
        {
            static auto* const callbacks = new ssh_jump_callbacks_struct{
                .userdata = nullptr,
                .before_connection = &beginJumpHost,
                .verify_knownhost = &verifyJumpHost,
                // libssh authenticates as it does without callbacks.
                .authenticate = nullptr,
            };
            return *callbacks;
        }

        std::string strictRefusal(HostKeyQuestion const& presented)
        {
            return fmt::format(
                "The host key of {} is in no known hosts file, and Strict Host Key Verification refuses unknown "
                "hosts.\nPresented key: {}\nAdd the key to {}, or turn Strict Host Key Verification off to be asked "
                "about unknown host keys.",
                serverOf(presented),
                keyOf(presented),
                knownHostsFileOf(presented)
            );
        }

        std::expected<void, std::string>
        askAboutUnknownKey(ssh_session session, HostKeyQuestion const& presented, HostKeySource const& askAboutHostKey)
        {
            const auto answer = askAboutHostKey ? askAboutHostKey(presented) : HostKeyAnswer::Refused;
            if (answer == HostKeyAnswer::Refused)
            {
                return std::unexpected{
                    fmt::format("The unknown host key of {} was not trusted: {}", serverOf(presented), keyOf(presented))
                };
            }

            Log::info("Trusting the host key of {}: {}", serverOf(presented), keyOf(presented));
            // The user trusted the key, so the connection goes on when only saving it fails, which they are shown.
            if (answer == HostKeyAnswer::Always && ssh_session_update_known_hosts(session) != SSH_OK)
            {
                Log::error(
                    "The host key of {} is trusted for now, but it could not be saved to {}: {}",
                    serverOf(presented),
                    knownHostsFileOf(presented),
                    ssh_get_error(session)
                );
            }
            return {};
        }
    }

    std::expected<void, std::string>
    verifyHostKey(ssh_session session, bool strict, HostKeySource const& askAboutHostKey)
    {
        // Before the lookup, which has nothing to compare without a key.
        const auto presented = presentedKeyOf(session);
        if (!presented)
            return std::unexpected{presented.error()};

        const auto found = lookUpStrictly(session, *presented);
        switch (found)
        {
            case SSH_KNOWN_HOSTS_OK:
                return {};
            case SSH_KNOWN_HOSTS_CHANGED:
            case SSH_KNOWN_HOSTS_OTHER:
                return refuseChangedKey(changedKeyOf(session, *presented, found), askAboutHostKey);
            case SSH_KNOWN_HOSTS_NOT_FOUND:
            case SSH_KNOWN_HOSTS_UNKNOWN:
                if (strict)
                    return std::unexpected{strictRefusal(*presented)};
                return askAboutUnknownKey(session, *presented, askAboutHostKey);
            case SSH_KNOWN_HOSTS_ERROR:
                break;
        }
        return std::unexpected{uncheckedKey(*presented, serverWording, session)};
    }

    std::optional<JumpHostVerification> JumpHostVerification::begin(ssh_session session, std::string knownHostsFile)
    {
        if (ssh_options_set(session, SSH_OPTIONS_PROXYJUMP_CB_LIST_APPEND, &jumpHostCallbacks()) != SSH_OK)
            return std::nullopt;

        auto record = std::make_shared<Record>();
        record->knownHostsFile = std::move(knownHostsFile);
        auto& goingOn = verificationGoingOn();
        std::scoped_lock lock{goingOn.mutex};
        goingOn.record = record;
        return JumpHostVerification{std::move(record)};
    }

    JumpHostVerification::JumpHostVerification(std::shared_ptr<Record> record)
        : record_{std::move(record)}
    {}

    JumpHostVerification::~JumpHostVerification()
    {
        if (!record_)
            return;
        auto& goingOn = verificationGoingOn();
        std::scoped_lock lock{goingOn.mutex};
        if (goingOn.record == record_)
            goingOn.record.reset();
    }

    JumpHostVerification::JumpHostVerification(JumpHostVerification&&) noexcept = default;

    JumpHostVerification& JumpHostVerification::operator=(JumpHostVerification&& other) noexcept
    {
        // The verification this one held ends with the temporary.
        JumpHostVerification ended{std::move(other)};
        std::swap(record_, ended.record_);
        return *this;
    }

    std::optional<JumpHostRefusal> JumpHostVerification::takeRefusal()
    {
        if (!record_)
            return std::nullopt;
        std::scoped_lock lock{record_->mutex};
        return std::exchange(record_->refusal, std::nullopt);
    }

    std::expected<void, JumpHostRefusal> verifyJumpHostKey(ssh_session jumpSession)
    {
        auto presented = presentedKeyOf(jumpSession);
        if (!presented)
            return refusedJumpHost({.jumpHost = true}, fmt::format("A jump host is refused: {}", presented.error()));
        presented->jumpHost = true;

        const auto found = lookUpStrictly(jumpSession, *presented);
        switch (found)
        {
            case SSH_KNOWN_HOSTS_OK:
                Log::debug("The host key of the jump host {} is known: {}", serverOf(*presented), keyOf(*presented));
                return {};
            case SSH_KNOWN_HOSTS_CHANGED:
            case SSH_KNOWN_HOSTS_OTHER:
            {
                const auto changed = changedKeyOf(jumpSession, *presented, found);
                return refusedJumpHost(changed, changedKeyRefusal(changed, jumpHostWording));
            }
            case SSH_KNOWN_HOSTS_NOT_FOUND:
            case SSH_KNOWN_HOSTS_UNKNOWN:
                return refusedJumpHost(*presented, unknownJumpHostKeyRefusal(*presented));
            case SSH_KNOWN_HOSTS_ERROR:
                break;
        }
        return refusedJumpHost(*presented, uncheckedKey(*presented, jumpHostWording, jumpSession));
    }
}
