add_library(core-target INTERFACE)

if (${MSVC})
    target_compile_options(core-target INTERFACE -Wmost)
else()
    target_compile_options(core-target INTERFACE -Wall -Wextra -Wpedantic)
endif()

set(MEM64 "")
set(EXCEPTIONS "")
if (EMSCRIPTEN)
    if (WIN32)
        set(MEM64 "-sMEMORY64=1")
    else()
        # No support for WASM64 in webkit
        set(MEM64 "-sWASM_BIGINT=1")
    endif()
    set(EXCEPTIONS "-fexceptions")
endif()

target_compile_options(core-target INTERFACE -Wbad-function-cast -Wcast-function-type -fexceptions -pedantic $<$<CONFIG:DEBUG>:-g;-Werror=return-type> $<$<CONFIG:RELEASE>:-O3> ${MEM64} ${EXCEPTIONS})
target_link_options(core-target INTERFACE $<$<CONFIG:RELEASE>:-s;-static-libgcc;-static-libstdc++> ${MEM64} ${EXCEPTIONS})
target_compile_features(core-target INTERFACE cxx_std_23)
target_compile_definitions(core-target INTERFACE JSON_DIAGNOSTICS=1)