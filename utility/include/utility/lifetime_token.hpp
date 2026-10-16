#pragma once

#include <memory>
#include <type_traits>
#include <utility>

namespace Utility
{
    /**
     * @brief Tells a callback whether the object that made it still exists. The object holds the token as a member
     *        and wraps callbacks that capture it with @ref guarded, which then do nothing once it is gone.
     *
     * A token belongs to the address of its owner: a copied or moved owner gets a new token, and assigning one owner
     * to another keeps each one's own token, so callbacks of the old address never run on the new one.
     */
    class LifetimeToken
    {
      public:
        LifetimeToken() = default;
        ~LifetimeToken() = default;
        LifetimeToken(LifetimeToken const&)
            : LifetimeToken{}
        {}
        LifetimeToken(LifetimeToken&&) noexcept
            : LifetimeToken{}
        {}
        LifetimeToken& operator=(LifetimeToken const&) noexcept
        {
            return *this;
        }
        LifetimeToken& operator=(LifetimeToken&&) noexcept
        {
            return *this;
        }

        /**
         * @brief @p callable wrapped so it runs only while this token exists. The wrapper takes the same parameters,
         *        so code that inspects them, like an RPC back channel, sees the original signature. @p callable has
         *        one call operator, which returns nothing: a call after the owner is gone has nothing to return.
         */
        template <typename CallableT>
        auto guarded(CallableT&& callable) const
        {
            return guardedFor(std::forward<CallableT>(callable), &std::remove_cvref_t<CallableT>::operator());
        }

        /**
         * @brief Checks whether the owner still exists, for callbacks that need the check without @ref guarded.
         */
        std::weak_ptr<bool const> watch() const
        {
            return alive_;
        }

      private:
        template <typename CallableT, typename ClassT, typename... ArgumentTs>
        auto guardedFor(CallableT&& callable, void (ClassT::*)(ArgumentTs...) const) const
        {
            return guardedWith<CallableT, ArgumentTs...>(std::forward<CallableT>(callable));
        }

        template <typename CallableT, typename ClassT, typename... ArgumentTs>
        auto guardedFor(CallableT&& callable, void (ClassT::*)(ArgumentTs...)) const
        {
            return guardedWith<CallableT, ArgumentTs...>(std::forward<CallableT>(callable));
        }

        template <typename CallableT, typename... ArgumentTs>
        auto guardedWith(CallableT&& callable) const
        {
            return [alive = std::weak_ptr<bool const>{alive_},
                       callable = std::forward<CallableT>(callable)](ArgumentTs... arguments) mutable
            {
                if (const auto locked = alive.lock(); !locked)
                    return;
                callable(std::forward<ArgumentTs>(arguments)...);
            };
        }

      private:
        std::shared_ptr<bool const> alive_{std::make_shared<bool>(true)};
    };
}
