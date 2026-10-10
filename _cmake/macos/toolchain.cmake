# Builds with an upstream LLVM and the static libc++ from scripts/macos/build_dependencies.sh instead of
# AppleClang and the system libc++, so the C++ version is not tied to Xcode. Every C++ dependency must come
# from the same prefix: libc++ uses its own ABI namespace there, mixing in a library built against the
# system libc++ fails to link.
#
# NUI_SFTP_LLVM_ROOT          LLVM install, defaults to `brew --prefix llvm@23`
# NUI_SFTP_DEPENDENCY_PREFIX  output of build_dependencies.sh, defaults to ~/.cache/nui-sftp/macos-dependencies
# Both can also come from the environment.

set(NUI_SFTP_LLVM_FORMULA "llvm@23")
set(NUI_SFTP_DEFAULT_MINIMUM_MACOS "15.2")

if (NOT NUI_SFTP_LLVM_ROOT)
    if (DEFINED ENV{NUI_SFTP_LLVM_ROOT})
        set(NUI_SFTP_LLVM_ROOT "$ENV{NUI_SFTP_LLVM_ROOT}")
    else()
        execute_process(
            COMMAND brew --prefix ${NUI_SFTP_LLVM_FORMULA}
            OUTPUT_VARIABLE NUI_SFTP_LLVM_ROOT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            COMMAND_ERROR_IS_FATAL ANY
        )
    endif()
endif()
if (NOT NUI_SFTP_DEPENDENCY_PREFIX)
    if (DEFINED ENV{NUI_SFTP_DEPENDENCY_PREFIX})
        set(NUI_SFTP_DEPENDENCY_PREFIX "$ENV{NUI_SFTP_DEPENDENCY_PREFIX}")
    else()
        set(NUI_SFTP_DEPENDENCY_PREFIX "$ENV{HOME}/.cache/nui-sftp/macos-dependencies")
    endif()
endif()
set(NUI_SFTP_LLVM_ROOT "${NUI_SFTP_LLVM_ROOT}" CACHE PATH "LLVM used for the macOS build")
set(NUI_SFTP_DEPENDENCY_PREFIX "${NUI_SFTP_DEPENDENCY_PREFIX}" CACHE PATH "Prefix of build_dependencies.sh")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES NUI_SFTP_LLVM_ROOT NUI_SFTP_DEPENDENCY_PREFIX)

if (NOT EXISTS "${NUI_SFTP_DEPENDENCY_PREFIX}/lib/libc++.a")
    message(FATAL_ERROR "No libc++ in ${NUI_SFTP_DEPENDENCY_PREFIX}, run scripts/macos/build_dependencies.sh first.")
endif()

set(CMAKE_C_COMPILER "${NUI_SFTP_LLVM_ROOT}/bin/clang")
set(CMAKE_CXX_COMPILER "${NUI_SFTP_LLVM_ROOT}/bin/clang++")
set(CMAKE_OBJC_COMPILER "${NUI_SFTP_LLVM_ROOT}/bin/clang")
set(CMAKE_OBJCXX_COMPILER "${NUI_SFTP_LLVM_ROOT}/bin/clang++")

set(CMAKE_OSX_DEPLOYMENT_TARGET "${NUI_SFTP_DEFAULT_MINIMUM_MACOS}" CACHE STRING "Oldest macOS the build runs on")
if (NOT CMAKE_OSX_SYSROOT)
    execute_process(
        COMMAND xcrun --sdk macosx --show-sdk-path
        OUTPUT_VARIABLE NUI_SFTP_MACOS_SDK
        OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY
    )
    set(CMAKE_OSX_SYSROOT "${NUI_SFTP_MACOS_SDK}" CACHE PATH "macOS SDK")
endif()

foreach(language CXX OBJCXX)
    string(APPEND CMAKE_${language}_FLAGS_INIT " -stdlib++-isystem ${NUI_SFTP_DEPENDENCY_PREFIX}/include/c++/v1")
    set(CMAKE_${language}_STANDARD_LIBRARIES_INIT
        "${NUI_SFTP_DEPENDENCY_PREFIX}/lib/libc++.a ${NUI_SFTP_DEPENDENCY_PREFIX}/lib/libc++abi.a"
    )
endforeach()
foreach(kind EXE SHARED MODULE)
    string(APPEND CMAKE_${kind}_LINKER_FLAGS_INIT " -nostdlib++")
endforeach()

list(PREPEND CMAKE_PREFIX_PATH "${NUI_SFTP_DEPENDENCY_PREFIX}")
set(CMAKE_FIND_FRAMEWORK LAST)
set(OPENSSL_ROOT_DIR "${NUI_SFTP_DEPENDENCY_PREFIX}")
set(OPENSSL_USE_STATIC_LIBS TRUE)
set(Boost_USE_STATIC_LIBS ON)
# roar looks for cryptopp in the Homebrew prefix.
set(BREW_PREFIX "${NUI_SFTP_DEPENDENCY_PREFIX}" CACHE STRING "Prefix roar takes cryptopp from" FORCE)
