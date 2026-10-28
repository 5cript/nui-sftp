#pragma once

#include <expected>

namespace SecureShell
{
    /**
     * @brief Makes what the future of a promise task holds when the task does not finish: its push was refused, the
     *        thread dropped it, or it threw. Defined for void (nothing), bool (false) and std::expected of an error
     *        that defines it (that error), other results pass their own.
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

    template <typename ValueT, typename ErrorT>
    struct UnfinishedTaskResult<std::expected<ValueT, ErrorT>>
    {
        std::expected<ValueT, ErrorT> operator()() const
        {
            return std::unexpected(UnfinishedTaskResult<ErrorT>{}());
        }
    };
}
