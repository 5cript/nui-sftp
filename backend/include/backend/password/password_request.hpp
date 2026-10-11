#pragma once

#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>

/**
 * @brief A password request that is resolved once, by a provider answer or by cancellation.
 */
class PasswordRequest
{
  public:
    PasswordRequest() = default;
    ~PasswordRequest() = default;
    PasswordRequest(PasswordRequest const&) = delete;
    PasswordRequest& operator=(PasswordRequest const&) = delete;
    PasswordRequest(PasswordRequest&&) = delete;
    PasswordRequest& operator=(PasswordRequest&&) = delete;

    /**
     * @brief Resolves the request and wakes the waiter. Calls after the first are ignored.
     *
     * @param password The password, or std::nullopt when there is none.
     */
    void resolve(std::optional<std::string> password);

    /**
     * @brief Blocks until the request is resolved.
     *
     * @return The password, or std::nullopt when there is none.
     */
    std::optional<std::string> wait();

  private:
    std::mutex mutex_{};
    std::condition_variable resolvedCondition_{};
    bool resolved_{false};
    std::optional<std::string> password_{};
};
