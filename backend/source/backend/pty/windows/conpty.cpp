#include <backend/pty/windows/conpty.hpp>
#include <backend/pty/windows/pipe.hpp>
#include <nui/utility/scope_exit.hpp>

#include <windows.h>
#include <WinCon.h>

#include <thread>
#include <atomic>

namespace bp2 = boost::process::v2;

using HPCON = void*;

using CreatePseudoConsoleFn =
    HRESULT(__stdcall*)(COORD size, HANDLE hInput, HANDLE hOutput, DWORD dwFlags, HPCON* phPC);
using ResizePseudoConsoleFn = HRESULT(__stdcall*)(HPCON hPC, COORD size);
using ClosePseudoConsoleFn = HRESULT(__stdcall*)(HPCON hPC);

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
constexpr static int ProcThreadAttributePseudoConsole = 22;
#    define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE \
        ProcThreadAttributeValue(ProcThreadAttributePseudoConsole, FALSE, TRUE, FALSE)
#endif

namespace ConPTY
{
    struct PseudoConsole::Implementation
    {
        HPCON consoleHandle;
        Pipe inputPipe;
        Pipe outputPipe;
        HMODULE kernel32;
        CreatePseudoConsoleFn createPseudoConsole;
        ResizePseudoConsoleFn resizePseudoConsole;
        ClosePseudoConsoleFn closePseudoConsole;

        std::thread readerThread;
        bool isReading;
        std::atomic_bool shallStopReading;

        Implementation()
            : consoleHandle{nullptr}
            , inputPipe{}
            , outputPipe{}
            , kernel32{LoadLibrary("kernel32.dll")}
#pragma clang diagnostic push
// This is winapi for you.
#pragma clang diagnostic ignored "-Wcast-function-type-strict"
#pragma clang diagnostic ignored "-Wcast-function-type-mismatch"
            , createPseudoConsole{reinterpret_cast<CreatePseudoConsoleFn>(
                  GetProcAddress(kernel32, "CreatePseudoConsole"))}
            , resizePseudoConsole{reinterpret_cast<ResizePseudoConsoleFn>(
                  GetProcAddress(kernel32, "ResizePseudoConsole"))}
            , closePseudoConsole{reinterpret_cast<ClosePseudoConsoleFn>(GetProcAddress(kernel32, "ClosePseudoConsole"))}
#pragma clang diagnostic pop
            , readerThread{}
            , isReading{false}
            , shallStopReading{false}
        {}
        ~Implementation()
        {
            FreeLibrary(kernel32);
        }
        Implementation(const Implementation&) = delete;
        Implementation& operator=(const Implementation&) = delete;
        Implementation(Implementation&&) = delete;
        Implementation& operator=(Implementation&&) = delete;
    };

    PseudoConsole::PseudoConsole()
        : impl_(std::make_unique<Implementation>())
    {}
    ROAR_PIMPL_SPECIAL_FUNCTIONS_IMPL_NO_DTOR(PseudoConsole);

    PseudoConsole::~PseudoConsole()
    {
        if (!moveDetector_.wasMoved())
        {
            impl_->closePseudoConsole(impl_->consoleHandle);
            if (impl_->isReading)
                stopReading();
        }
    }

    void PseudoConsole::closeOtherPipeEnd()
    {
        impl_->inputPipe.closeReadSide();
        impl_->outputPipe.closeWriteSide();
    }

    bool PseudoConsole::initialize()
    {
        HRESULT result{S_OK};
        impl_->inputPipe.applyReadWrite<HANDLE>([this, &result](HANDLE inputRead, HANDLE) {
            impl_->outputPipe.applyReadWrite<HANDLE>([this, &result, inputRead](HANDLE, HANDLE outputWrite) {
                // TODO: do not hardcode the size
                result = impl_->createPseudoConsole({80, 30}, inputRead, outputWrite, 0, &impl_->consoleHandle);
            });
        });
        return FAILED(result) ? false : true;
    }

    void PseudoConsole::stopReading()
    {
        impl_->isReading = false;
        impl_->shallStopReading = true;
        if (impl_->readerThread.joinable())
            impl_->readerThread.join();
        impl_->outputPipe.closeReadSide();
    }

    void PseudoConsole::startReading(
        std::function<void(std::string_view)> onStdout,
        std::function<void(std::string_view)> onStderr)
    {
        impl_->isReading = true;
        impl_->shallStopReading = false;
        impl_->readerThread = std::thread{[this, onStdout = std::move(onStdout), onStderr = std::move(onStderr)]() {
            std::vector<char> buffer(4096);
            while (!impl_->shallStopReading)
            {
                std::size_t bytesRead{0};
                impl_->outputPipe.read(buffer, bytesRead);
                if (bytesRead == 0)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
                if (onStdout)
                    onStdout(std::string_view{buffer.data(), bytesRead});
            }
        }};
    }

    bool PseudoConsole::write(std::string_view data)
    {
        return impl_->inputPipe.write(data);
    }

    bool PseudoConsole::prepareProcessLauncher(boost::process::v2::windows::default_launcher& launcher)
    {
        auto& si = launcher.startup_info;

        size_t bytesRequired;
        InitializeProcThreadAttributeList(NULL, 1, 0, &bytesRequired);

        si.lpAttributeList =
            reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(), 0, bytesRequired));
        if (!si.lpAttributeList)
            return false;

        Nui::ScopeExit cleanupLpAttributeList{[&si]() noexcept {
            HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
        }};

        si.StartupInfo.hStdInput = 0;
        si.StartupInfo.hStdOutput = 0;
        si.StartupInfo.hStdError = 0;
        si.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;

        if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &bytesRequired))
            return false;

        if (!UpdateProcThreadAttribute(
                si.lpAttributeList,
                0,
                PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                impl_->consoleHandle,
                sizeof(HPCON),
                nullptr,
                nullptr))
        {
            return false;
        }

        cleanupLpAttributeList.disarm();
        return true;
    }

    void PseudoConsole::resize(short width, short height)
    {
        width = std::max(static_cast<short>(1), width);
        height = std::max(static_cast<short>(1), height);

        impl_->resizePseudoConsole(impl_->consoleHandle, {width, height});
    }

    PseudoConsole createPseudoConsole()
    {
        auto pseudoConsole{PseudoConsole{}};
        pseudoConsole.initialize();
        return pseudoConsole;
    }
}