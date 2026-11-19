#pragma once

#include <concepts>
#include <cstddef>
#include <functional>
#include <type_traits>
#include <utility>

namespace FileTransferProtocol
{
    template <typename SignatureT>
    class NoexceptFunction;

    /**
     * @brief A std::function that only takes what is declared noexcept. A callable that throws all the same ends the
     *        program at its call, so the caller has no exception to clean up behind.
     */
    template <typename ResultT, typename... ArgumentTs>
    class NoexceptFunction<ResultT(ArgumentTs...)>
    {
      public:
        NoexceptFunction() = default;
        NoexceptFunction(std::nullptr_t)
        {}

        template <typename CallableT>
            requires(!std::same_as<std::remove_cvref_t<CallableT>, NoexceptFunction>) &&
                    std::is_nothrow_invocable_r_v<ResultT, std::decay_t<CallableT>&, ArgumentTs...>
        NoexceptFunction(CallableT&& callable)
            : function_{std::forward<CallableT>(callable)}
        {}

        ResultT operator()(ArgumentTs... arguments) const noexcept
        {
            return function_(std::forward<ArgumentTs>(arguments)...);
        }

        explicit operator bool() const noexcept
        {
            return static_cast<bool>(function_);
        }

      private:
        std::function<ResultT(ArgumentTs...)> function_{};
    };
}
