#pragma once

#include <nui/rpc.hpp>
#include <nlohmann/json.hpp>
#include <fmt/format.h>
#include <log/log.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <traits/functions.hpp>
#include <mplex/tuple/pop_front.hpp>

#include <string>
#include <concepts>
#include <ranges>
#include <memory>
#include <chrono>

// TODO: split and document this file!
namespace RpcHelper
{
    namespace Detail
    {
        template <typename ArgsTuple>
        struct CallOperatorApplyTypesOnly
        {};

        template <typename... Args>
        struct CallOperatorApplyTypesOnly<std::tuple<Args...>>
        {
            template <typename F>
            static void apply(F&& func)
            {
                return std::forward<F>(func).template operator()<Args...>();
            }
        };

        template <typename>
        struct DebugPrintType;
    }

    class RpcInCorrectThread
    {
      public:
        RpcInCorrectThread(Nui::Window& wnd, Nui::RpcHub& hub, std::string const& responseId)
            : wnd_{&wnd}
            , hub_{&hub}
            , responseId_{responseId}
        {}
        RpcInCorrectThread(RpcInCorrectThread&&) = default;
        RpcInCorrectThread& operator=(RpcInCorrectThread&&) = default;
        RpcInCorrectThread(RpcInCorrectThread const&) = default;
        RpcInCorrectThread& operator=(RpcInCorrectThread const&) = default;
        ~RpcInCorrectThread() = default;

        void operator()(nlohmann::json&& json) const
        {
            wnd_->runInJavascriptThread([this, json = std::move(json)]() {
                try
                {
                    hub_->callRemote(responseId_, json);
                }
                catch (const std::exception& e)
                {
                    Log::error("Failed to call rpc respond '{}': {}", responseId_, e.what());
                }
            });
        }

      protected:
        Nui::Window* wnd_;
        Nui::RpcHub* hub_;
        std::string responseId_;
    };

    class RpcOnce
    {
      public:
        RpcOnce(Nui::Window& wnd, Nui::RpcHub& hub, std::string const& responseId)
            : wnd_{&wnd}
            , hub_{&hub}
            , responseId_{responseId}
        {}
        RpcOnce(RpcOnce&&) = default;
        RpcOnce& operator=(RpcOnce&&) = default;
        RpcOnce(RpcOnce const&) = delete;
        RpcOnce& operator=(RpcOnce const&) = delete;
        ~RpcOnce() = default;

        void operator()(nlohmann::json&& json) const
        {
            if (called_)
            {
                Log::warn("RPC response with id '{}' already sent. Not sending: {}", responseId_, json.dump(4));
                return;
            }
            called_ = true;

            wnd_->runInJavascriptThread([hub = hub_, responseId = std::move(responseId_), json = std::move(json)]() {
                try
                {
                    hub->callRemote(responseId, json);
                }
                catch (const std::exception& e)
                {
                    Log::error("Failed to call rpc respond '{}': {}", responseId, e.what());
                }
            });
        }

        void error(std::string const& message) const
        {
            if (called_)
            {
                Log::warn("RPC response with id '{}' already sent. Not sending error: {}", responseId_, message);
                return;
            }
            called_ = true;

            wnd_->runInJavascriptThread([hub = hub_, responseId = std::move(responseId_), message]() {
                try
                {
                    hub->callRemote(responseId, {{"success", false}, {"error", message}});
                }
                catch (const std::exception& e)
                {
                    Log::error("Failed to call rpc respond '{}': {}", responseId, e.what());
                }
            });
        }

      private:
        Nui::Window* wnd_;
        Nui::RpcHub* hub_;
        mutable std::string responseId_;
        mutable bool called_{false};
    };

    class ParameterVerifyView
    {
      private:
        RpcOnce& rpcOnce_;
        nlohmann::json const& json_;
        std::string const functionName_;

      public:
        ParameterVerifyView(RpcOnce& rpcOnce, std::string const& functionName, nlohmann::json const& json)
            : rpcOnce_{rpcOnce}
            , json_{json}
            , functionName_{functionName}
        {}
        ~ParameterVerifyView() = default;
        ParameterVerifyView(ParameterVerifyView const&) = delete;
        ParameterVerifyView& operator=(ParameterVerifyView const&) = delete;

        // Lets not have this view moveable somewhere, to avoid it being used after the original json is gone
        ParameterVerifyView(ParameterVerifyView&&) = delete;
        ParameterVerifyView& operator=(ParameterVerifyView&&) = delete;

        template <typename KeyT, typename... KeysT>
        requires(std::convertible_to<KeyT, std::string_view> && (std::convertible_to<KeysT, std::string_view> && ...))
        bool hasValueDeep(KeyT&& key, KeysT&&... keys)
        {
            std::vector<std::string> keyChain{};

            auto onFail = [this, &keyChain]() {
                keyChain.pop_back();
                rpcOnce_({
                    {
                        "error",
                        fmt::format(
                            "Missing parameter to function '{}': {}",
                            functionName_,
                            keyChain | std::views::join | std::ranges::to<std::string>()),
                    },
                });
                return false;
            };

            auto iter = json_.find(key);
            auto currentEnd = end(json_);

            auto checkOnce = [&iter, &keyChain, &onFail, &currentEnd](std::string_view key) {
                keyChain.push_back(std::string{key});
                keyChain.push_back(".");
                if (iter == currentEnd)
                    return onFail();
                currentEnd = end(*iter);
                return true;
            };

            auto findNext = [&iter, &checkOnce](std::string_view key) {
                iter = iter->find(key);
                return checkOnce(key);
            };

            return checkOnce(key) && (findNext(keys) && ...);
        }
    };

    template <typename FunctionT>
    auto rpcSafe(RpcOnce&& rpcOnce, FunctionT&& func)
    {
        return [rpcOnce = std::make_shared<RpcOnce>(std::move(rpcOnce)),
                func = std::forward<FunctionT>(func)](auto&&... args) mutable {
            try
            {
                if constexpr (std::is_invocable_v<FunctionT, RpcOnce&&, decltype(args)...>)
                    func(std::move(*rpcOnce), std::forward<decltype(args)>(args)...);
                else if constexpr (std::is_invocable_v<FunctionT, decltype(args)...>)
                    func(std::forward<decltype(args)>(args)...);
                else
                    static_assert(false, "FunctionT is not callable with the given arguments");
            }
            catch (const std::exception& e)
            {
                (*rpcOnce)({{"error", e.what()}});
            }
        };
    }

    class StrandRpc
    {
      public:
        StrandRpc(boost::asio::any_io_executor executor, Nui::Window& wnd, Nui::RpcHub& hub)
            : executor_{executor}
            , strand_{std::make_shared<boost::asio::strand<boost::asio::any_io_executor>>(executor_)}
            , wnd_{&wnd}
            , hub_{&hub}
            , timer_{executor_}
        {}

        StrandRpc(
            boost::asio::any_io_executor executor,
            std::shared_ptr<boost::asio::strand<boost::asio::any_io_executor>> strand,
            Nui::Window& wnd,
            Nui::RpcHub& hub)
            : executor_{executor}
            , strand_{std::move(strand)}
            , wnd_{&wnd}
            , hub_{&hub}
            , timer_{executor_}
        {}

        virtual ~StrandRpc() = default;
        StrandRpc(StrandRpc const&) = delete;
        StrandRpc& operator=(StrandRpc const&) = delete;
        StrandRpc(StrandRpc&&) = default;
        StrandRpc& operator=(StrandRpc&&) = default;

        template <typename FunctionT>
        void registerOnStrand(std::string_view functionName, FunctionT&& func)
        {
            // (RpcReply&&, Param1, Param2, ...) => (Param1, Param2, ...)
            using ArgsTuple = mplex::pop_front_t<typename Traits::FunctionTraits<std::decay_t<FunctionT>>::ArgsTuple>;

            // Call lambda with template arguments FunctionT, Param1, Param2, ... and no parameters.
            Detail::CallOperatorApplyTypesOnly<ArgsTuple>::apply([this,
                                                                  functionName = std::string{functionName},
                                                                  func = std::forward<FunctionT>(
                                                                      func)]<typename... ParameterTs>() mutable {
                // Register function that is wrapped in a strand execute call:
                registeredFunctions_.emplace_back(hub_->autoRegisterFunction(
                    functionName,
                    [this, func = std::move(func), functionName](std::string responseId, ParameterTs&&... parameters) {
                        strand_->execute([responseId = std::move(responseId),
                                          ... parameters = std::forward<ParameterTs>(parameters),
                                          func,
                                          wnd = wnd_,
                                          hub = hub_]() mutable {
                            // Threadsafe do:
                            RpcHelper::rpcSafe(
                                RpcHelper::RpcOnce{*wnd, *hub, std::move(responseId)},
                                [&parameters..., &func](RpcOnce&& reply) mutable {
                                    // Call actual function
                                    func(std::move(reply), std::forward<ParameterTs>(parameters)...);
                                })();
                        });
                    }));
            });
        }

        class Proxy
        {
          private:
            StrandRpc* parent_;
            std::string functionName;

          public:
            Proxy(StrandRpc* parent, std::string functionName)
                : parent_(parent)
                , functionName(std::move(functionName))
            {}

            template <typename... Args>
            void perform(Args&&... args)
            {
                parent_->registerOnStrand(functionName, std::forward<Args>(args)...);
            }
        };
        Proxy on(std::string_view functionName)
        {
            return Proxy{this, std::string{functionName}};
        }

        void within_strand_do(auto&& func) const
        {
            if (!strand_->running_in_this_thread())
                return strand_->execute(std::forward<decltype(func)>(func));
            func();
        }

        void within_strand_do_no_recurse(auto&& func) const
        {
            return strand_->execute(std::forward<decltype(func)>(func));
        }

        void within_strand_do_delayed(auto&& func, std::chrono::steady_clock::duration delay)
        {
            timer_.expires_after(delay);
            timer_.async_wait([func = std::forward<decltype(func)>(func)](auto const& ec) {
                if (ec)
                    Log::info("Timer canceled in within_strand_do_delayed: {}", ec.message());

                // Run func anyway, if its because of a shutdown, it will stop early.
                func();
            });
        }

      protected:
        boost::asio::any_io_executor executor_{};
        std::shared_ptr<boost::asio::strand<boost::asio::any_io_executor>> strand_;
        Nui::Window* wnd_;
        Nui::RpcHub* hub_;
        std::vector<Nui::RpcHub::AutoUnregister> registeredFunctions_{};
        boost::asio::steady_timer timer_;
    };
}