#pragma once

#include <remote_io/remote_error.hpp>

#include <expected>

namespace RemoteIo
{
    /**
     * @brief Makes what the future of a promise task holds when the task does not finish: its push was refused, the
     *        thread dropped it, or it threw. Defined for void (nothing), bool (false), RemoteError (a failed task push)
     *        and std::expected of an error that defines it (that error), other results pass their own.
     */
    template <typename ResultT>
    struct UnfinishedTaskResult;

    template <>
    struct UnfinishedTaskResult<void>
    {
        void operator()() const noexcept
        {}
    };

    template <>
    struct UnfinishedTaskResult<bool>
    {
        bool operator()() const noexcept
        {
            return false;
        }
    };

    template <>
    struct UnfinishedTaskResult<RemoteError>
    {
        RemoteError operator()() const
        {
            return RemoteError{
                .message = "The task did not finish, the connection is closed or its thread stopped",
                .wrapperError = WrapperErrors::TaskPushFailed,
            };
        }
    };

    template <typename ValueT, typename ErrorT>
    struct UnfinishedTaskResult<std::expected<ValueT, ErrorT>>
    {
        std::expected<ValueT, ErrorT> operator()() const
        {
            return std::unexpected(UnfinishedTaskResult<ErrorT>{}());
        }
    };
}
