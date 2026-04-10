#include <backend/process/process.hpp>

#include <backend/process/boost_process.hpp>
#include <boost/asio.hpp>

#include <mutex>
#include <condition_variable>
#include <iomanip>
#include <iostream>
#include <map>

namespace bp2 = boost::process::v2;

struct Process::Implementation
{
    boost::asio::any_io_executor executor;
    std::unique_ptr<bp2::process> child;
    boost::asio::steady_timer exitWaitTimer;

    std::chrono::seconds defaultExitWaitTimeout;
    std::optional<int> exitCode;

    std::function<bool(std::string_view)> onStdout;
    std::function<bool(std::string_view)> onStderr;
    // Only used on Windows:
    std::function<void()> onExit;

    std::vector<char> stdoutBuffer;
    std::vector<char> stderrBuffer;

    boost::asio::readable_pipe stdoutPipe;
    boost::asio::readable_pipe stderrPipe;
    boost::asio::writable_pipe stdinPipe;

    std::mutex awaitExit;
    std::condition_variable exitCondition;
    bool exited;
    bool exitRequested{false};

    std::map<int, std::unique_ptr<void, void (*)(void*)>> stateCache;

    Implementation(boost::asio::any_io_executor executor, std::function<void()> onExit)
        : executor{std::move(executor)}
        , exitWaitTimer{this->executor}
        , defaultExitWaitTimeout{10}
        , exitCode{}
        , onStdout{}
        , onStderr{}
        , onExit{std::move(onExit)}
        , stdoutBuffer(4096)
        , stderrBuffer(4096)
        , stdoutPipe{this->executor}
        , stderrPipe{this->executor}
        , stdinPipe{this->executor}
        , awaitExit{}
        , exitCondition{}
        , exited{false}
        , stateCache{}
    {}

    void read(
        std::shared_ptr<Process> proc,
        boost::asio::readable_pipe Process::Implementation::* pipe,
        std::function<bool(std::string_view)> Process::Implementation::* onRead,
        std::vector<char> Process::Implementation::* buffer
    )
    {
        auto& pipeRef = proc->impl_.get()->*pipe;
        pipeRef.async_read_some(
            boost::asio::buffer(proc->impl_.get()->*buffer),
            [weak = proc->weak_from_this(), pipe, onRead, buffer](
                boost::system::error_code ec, std::size_t bytesTransferred
            ) mutable
            {
                auto self = weak.lock();
                if (!self)
                    return;

                auto const& buf = self->impl_.get()->*buffer;
                auto const& whenRead = self->impl_.get()->*onRead;

                if (bytesTransferred > 0 && !whenRead(std::string_view{buf.data(), bytesTransferred}))
                    return;

                if (ec)
                    return;

                {
                    std::scoped_lock lock{self->impl_->awaitExit};
                    if (self->impl_->exited)
                        return;
                }

                self->impl_->read(self, pipe, onRead, buffer);
            }
        );
    }

    void write(std::shared_ptr<Process> proc, std::shared_ptr<std::string>&& data)
    {
        if (data->empty())
            return;

        boost::asio::async_write(
            proc->impl_->stdinPipe,
            boost::asio::buffer(*data),
            [weak = proc->weak_from_this(),
                data = std::move(data)](boost::system::error_code ec, std::size_t bytesTransferred) mutable
            {
                if (ec)
                    return;

                auto self = weak.lock();
                if (!self)
                    return;

                *data = data->substr(bytesTransferred);

                self->impl_->write(self, std::move(data));
            }
        );
    }

    void notifyExit()
    {
        {
            std::scoped_lock lock{awaitExit};
            exited = true;
        }
        exitCondition.notify_all();
    }

    bool isRunning() const
    {
        return child && child->running();
    }
};

Process::Process(boost::asio::any_io_executor executor, std::function<void()> onExit)
    : impl_{std::make_unique<Implementation>(std::move(executor), std::move(onExit))}
{}
Process::~Process()
{
    try
    {
        if (moveDetector_.wasMoved())
            return;
        exitSync(impl_->defaultExitWaitTimeout);
        impl_->stateCache.clear();
    }
    catch (...)
    {}
}
ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL_NO_DTOR(Process);

std::optional<int> Process::exitSync(std::optional<std::chrono::seconds> exitWaitTimeout)
{
    const auto waitTimeout = exitWaitTimeout.value_or(impl_->defaultExitWaitTimeout);
    if (!exit(waitTimeout))
        return impl_->exitCode;

    {
        std::unique_lock lock{impl_->awaitExit};
        impl_->exitCondition.wait_for(
            lock,
            waitTimeout * 2,
            [this]
            {
                return impl_->exited;
            }
        );
    }
    return impl_->exitCode;
}

boost::process::v2::pid_type Process::pid() const
{
    if (!impl_->child)
        return 0;
    return impl_->child->id();
}

bool Process::exit(std::optional<std::chrono::seconds> exitWaitTimeout)
{
    if (!impl_->child)
        return false;
    if (impl_->exitRequested)
        return false;
    if (!impl_->child->running())
    {
        if (!impl_->exitCode)
        {
            impl_->exitRequested = true;
            boost::system::error_code ec;
            impl_->child->wait(ec);
            impl_->exitCode = impl_->child->exit_code();
            return false;
        }
        return false;
    }

    {
        std::scoped_lock lock{impl_->awaitExit};
        impl_->exited = false;
    }

    impl_->exitRequested = true;
    impl_->child->request_exit();
    impl_->exitWaitTimer.expires_after(exitWaitTimeout.value_or(impl_->defaultExitWaitTimeout));
    impl_->exitWaitTimer.async_wait(
        [weak = weak_from_this()](boost::system::error_code ec)
        {
            if (ec)
                return;

            auto self = weak.lock();
            if (!self)
                return;

            self->impl_->child->terminate();
        }
    );
    impl_->child->async_wait(
        [weak = weak_from_this()](auto ec, auto code)
        {
            auto self = weak.lock();
            if (!self)
                return;

            self->impl_->exitWaitTimer.cancel();
            if (ec)
            {
                return self->terminate();
            }

            self->impl_->exitCode = code;
            self->impl_->notifyExit();
        }
    );

    return true;
}

#ifdef __linux__
void Process::signal(int signal)
{
    kill(impl_->child->id(), signal);
}
#endif

void Process::terminate()
{
    if (!impl_->isRunning())
        return;

    impl_->child->terminate();
    impl_->child->wait();
    impl_->exitCode = impl_->child->exit_code();
    impl_->notifyExit();
}

std::optional<int> Process::exitCode() const
{
    return impl_->exitCode;
}

void Process::attachState(int key, std::unique_ptr<void, void (*)(void*)> state)
{
    impl_->stateCache.emplace(key, std::move(state));
}

void* Process::getState(int key) const
{
    auto iter = impl_->stateCache.find(key);
    if (iter == impl_->stateCache.end())
        return nullptr;
    return iter->second.get();
}

void Process::spawn(
    std::string const& processName,
    std::vector<std::string> const& arguments,
    Environment environment,
    std::chrono::seconds defaultExitWaitTimeout,
    std::function<std::unique_ptr<boost::process::v2::process>(
        boost::asio::any_io_executor,
        boost::filesystem::path const& executable,
        std::vector<std::string> const& args,
        std::unordered_map<bp2::environment::key, bp2::environment::value>
    )> launcher
)
{
    impl_->defaultExitWaitTimeout = defaultExitWaitTimeout;
    if (impl_->isRunning())
    {
        exitSync(impl_->defaultExitWaitTimeout);
    }

    impl_->child.reset();

    std::unordered_map<bp2::environment::key, bp2::environment::value> env;
    for (auto const& [key, value] : environment.environment())
    {
        if (key.empty() || value.empty())
            continue;
        env.emplace(key, value);
    }

    auto executable = bp2::environment::find_executable(processName, env);
    if (executable.empty())
        executable = processName;

    {
        std::scoped_lock lock{impl_->awaitExit};
        impl_->exited = false;
    }

    if (!launcher)
    {
        impl_->child = std::make_unique<bp2::process>(
            impl_->executor,
            executable,
            arguments,
            bp2::process_environment{env},
            bp2::process_stdio(impl_->stdinPipe, impl_->stdoutPipe, impl_->stderrPipe)
        );
    }
    else
    {
        impl_->child = launcher(impl_->executor, executable, arguments, env);
    }

#ifdef _WIN32
    if (impl_->child->running())
    {
        impl_->child->async_wait(
            [self = shared_from_this()](auto ec, auto code)
            {
                if (ec)
                    return;

                self->impl_->exitCode = code;
                self->impl_->notifyExit();
                if (self->impl_->onExit)
                    self->impl_->onExit();
            }
        );
    }
#endif
}

void Process::startReading(
    std::function<bool(std::string_view)> onStdout,
    std::function<bool(std::string_view)> onStderr
)
{
    impl_->onStdout = std::move(onStdout);
    impl_->onStderr = std::move(onStderr);

    impl_->read(
        shared_from_this(),
        &Process::Implementation::stdoutPipe,
        &Process::Implementation::onStdout,
        &Process::Implementation::stdoutBuffer
    );

    impl_->read(
        shared_from_this(),
        &Process::Implementation::stderrPipe,
        &Process::Implementation::onStderr,
        &Process::Implementation::stderrBuffer
    );
}

bool Process::running() const
{
    return impl_->isRunning();
}

void Process::write(std::string_view data)
{
    write(std::string{data});
}

void Process::write(std::string data)
{
    if (!impl_->isRunning())
        return;

    auto sharedData = std::make_shared<std::string>(std::move(data));
    impl_->write(shared_from_this(), std::move(sharedData));
}

void Process::write(char const* data)
{
    write(std::string{data});
}