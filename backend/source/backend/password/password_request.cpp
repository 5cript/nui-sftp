#include <backend/password/password_request.hpp>

void PasswordRequest::resolve(std::optional<std::string> password)
{
    {
        std::scoped_lock lock{mutex_};
        if (resolved_)
            return;
        resolved_ = true;
        password_ = std::move(password);
    }
    resolvedCondition_.notify_all();
}

std::optional<std::string> PasswordRequest::wait()
{
    std::unique_lock lock{mutex_};
    resolvedCondition_.wait(
        lock,
        [this]()
        {
            return resolved_;
        }
    );
    return password_;
}
