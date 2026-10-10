#pragma once

#include <backend/process/environment.hpp>

#ifdef __clang__
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Weverything"
#endif

#include <backend/process/boost_process.hpp>

#ifdef __clang__
#    pragma clang diagnostic pop
#endif

#include <boost/asio/any_io_executor.hpp>
#include <roar/detail/pimpl_special_functions.hpp>
#include <nui/utility/move_detector.hpp>
#include <boost/process/v2/environment.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <optional>
#include <utility>

class Process : public std::enable_shared_from_this<Process>
{
  public:
    Process(boost::asio::any_io_executor executor, std::function<void()> onExit);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(Process);

    void spawn(
        std::string const& processName,
        std::vector<std::string> const& arguments,
        Environment environment,
        std::chrono::seconds defaultExitWaitTimeout = std::chrono::seconds{10},
        std::function<std::unique_ptr<boost::process::v2::process>(
            boost::asio::any_io_executor,
            boost::filesystem::path const& executable,
            std::vector<std::string> const& args,
            std::unordered_map<boost::process::v2::environment::key, boost::process::v2::environment::value>)>
            launcher = {});

    /**
     * @brief Returns true if an async operation was started to exit the process.
     *
     * @param exitWaitTimeout
     * @return true
     * @return false
     */
    bool exit(std::optional<std::chrono::seconds> exitWaitTimeout = std::nullopt);
    std::optional<int> exitSync(std::optional<std::chrono::seconds> exitWaitTimeout = std::nullopt);

    void terminate();

#ifndef _WIN32
    void signal(int signal);
#endif

    std::optional<int> exitCode() const;
    boost::process::v2::pid_type pid() const;

    bool running() const;

    void startReading(std::function<bool(std::string_view)> onStdout, std::function<bool(std::string_view)> onStderr);

    void write(std::string_view data);
    void write(std::string data);
    void write(char const* data);

    template <typename KeyT, typename T>
    void attachState(KeyT key, T&& state)
    requires(!std::is_lvalue_reference_v<T> && (std::is_integral_v<KeyT> || std::is_enum_v<KeyT>))
    {
        std::unique_ptr<void, void (*)(void*)> statePtr{new T{std::forward<T>(state)}, [](void* ptr) {
                                                            delete static_cast<T*>(ptr);
                                                        }};
        attachState(static_cast<int>(key), std::move(statePtr));
    }

    template <typename T, typename KeyT>
    T& getState(KeyT key)
    requires(std::is_integral_v<KeyT> || std::is_enum_v<KeyT>)
    {
        return *static_cast<T*>(getState(static_cast<int>(key)));
    }

  private:
    void attachState(int key, std::unique_ptr<void, void (*)(void*)> state);
    void* getState(int key) const;

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;

    Nui::MoveDetector moveDetector_;
};