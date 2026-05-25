# Contributing to nui-sftp

Thanks for your interest in nui-sftp. This document covers how to build the
project, where the moving pieces live, the conventions the code follows, and
how changes are validated in CI.

## About the project

nui-sftp is a cross-platform (Linux and Windows) SSH/SFTP workbench. The
frontend is written in C++ and compiled to WebAssembly via Emscripten using
[Nui](https://github.com/NuiCpp/Nui); the backend is a native C++ application
that hosts a WebKit/WebView2 webview and talks to libssh. Frontend and backend
communicate through Nui's RPC bridge.

Submodules pin most C++ dependencies. System packages (libssh, boost, fmt,
crypto++, webkitgtk on Linux) are pulled from the distro or MSYS2.

## Reporting bugs and asking questions

- Use the templates under [.github/ISSUE_TEMPLATE/](.github/ISSUE_TEMPLATE/) for
  bug reports, feature requests, and general questions.
- **Do not** file security issues publicly. The disclosure process is in
  [SECURITY.md](SECURITY.md).

## Getting the source

```bash
git clone https://github.com/5cript/nui-sftp.git
cd nui-sftp
./setup.sh   # equivalent to: git submodule update --init --recursive
```

All twelve submodules under [dependencies/](dependencies/) (Nui, roar, gimo,
traits, 5cript-nui-components, portable-file-dialogs, webview, yaml-cpp,
rapidfuzz, spdlog, efsw, promise-cpp) must be present before configuring.

## Toolchain requirements

The project uses C++23. CI builds with Clang and Ninja, and I recommend the
same locally. The full package list for each platform lives in the workflow
files; treat those as authoritative.

### Linux (Arch reference)

See [.github/workflows/archlinux.yml](.github/workflows/archlinux.yml). The
minimum packages CI installs are:

```
cmake ninja clang lld git python nodejs
webkitgtk-6.0 curl crypto++ libssh fmt boost boost-libs
nlohmann-json zlib bzip2 zstd xz
```

Node.js 24 is required for the frontend build (Nui invokes `parcel` and
`npm`).

### Windows (MSYS2 clang64)

See [.github/workflows/windows.yml](.github/workflows/windows.yml). Builds run
inside the `clang64` MSYS2 environment with:

```
mingw-w64-clang-x86_64-{clang,cmake,ninja,boost,crypto++,python,
                       pugixml,fmt,libssh,cppwinrt,zlib,bzip2,zstd,xz,
                       imagemagick,librsvg}
```

Visual Studio's MSVC toolchain is not supported.

## Building

All builds are plain CMake + Ninja invocations. I use VS Code tasks to drive them
locally; the editor configuration is not committed
(`.vscode/`, `.clangd`, and `build/` are all gitignored), so this section
documents the raw commands. A minimal `tasks.json` / `launch.json` you can drop
into your own `.vscode/` is provided at the end of this document.

### Debug build (frontend + backend)

```bash
cmake -B build/clang_debug -S . -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_CXX_STANDARD=23 \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DNUI_FETCH_TRAITS=OFF -DNUI_FETCH_ROAR=OFF \
  -DNUI_SFTP_ENABLE_TESTING=ON \
  -DNUI_BUILD_XML_TOOL=ON \
  -DNUI_SFT_ENABLE_LANGUAGE_TOOLING=ON

cmake --build build/clang_debug
```

The frontend WASM module is built as the `nui-sftp-emscripten` sub-target and
copied into `build/clang_debug/bin/../frontend/` as a post-build step, so the
debug binary at `build/clang_debug/bin/nui-sftp` finds it at runtime. Both
targets can be rebuilt individually:

```bash
cmake --build build/clang_debug --target nui-sftp-emscripten   # frontend only
cmake --build build/clang_debug --target nui-sftp              # backend only
```

### Release build

```bash
cmake -B build/clang_release -S . -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_CXX_STANDARD=23 \
  -DNUI_FETCH_TRAITS=OFF -DNUI_FETCH_ROAR=OFF \
  -DCMAKE_INSTALL_PREFIX="$PWD/install"

cmake --build   build/clang_release --target nui-sftp
# Installs into build/install:
./scripts/deploy.sh
```

The release configure omits tests and the XML tool by default.

### Standalone marketing webpage

A separate Emscripten module under [webpage/](webpage/) is built with
`-DBUILD_WEBPAGE=ON`. It produces its own `bin/` independent of the main
frontend:

```bash
cmake -B build/clang_webpage -S . -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_CXX_STANDARD=23 \
  -DNUI_FETCH_TRAITS=OFF -DNUI_FETCH_ROAR=OFF \
  -DBUILD_WEBPAGE=ON

cmake --build build/clang_webpage --target nui-sftp-webpage-emscripten
```

`webpage/scripts/serve.sh` and `webpage/scripts/build_and_deploy.sh` handle
local preview and remote deployment.

### Flatpak

The Flatpak is not built from this repository. Its manifest and build scripts
live in the companion [nui-sftp-deploy](https://github.com/5cript/nui-sftp-deploy)
repo alongside the PKGBUILD, AppImage Dockerfile, and AppStream metainfo. To
build a Flatpak locally:

```bash
git clone https://github.com/5cript/nui-sftp-deploy.git
cd nui-sftp-deploy
./scripts/build_flatpak.sh         # produces build/nui-sftp.flatpak
flatpak install --user --reinstall build/nui-sftp.flatpak
flatpak run org.nuicpp.nui_sftp
```

The manifest pins `OFFLINE_BUILD=ON` and `OMIT_FRONTEND_BUILD=ON`; the frontend
ships as a pre-built artifact (the `nui-sftp-frontend` tarball from the
matching release) rather than being compiled inside the sandbox.

## Useful CMake options

These are declared at the top of [CMakeLists.txt](CMakeLists.txt). The defaults
are sensible; override them only when needed.

| Option | Default | Purpose |
| --- | --- | --- |
| `NUI_SFTP_ENABLE_TESTING` | `OFF` | Build the GoogleTest suites under `*/test/` |
| `NUI_SFT_ENABLE_LANGUAGE_TOOLING` | `OFF` | Build the `language-check` CLI tool |
| `NUI_BUILD_XML_TOOL` | (via Nui) | Build the `xml-to-nui` SVG/XML converter |
| `OMIT_FRONTEND_BUILD` | `OFF` | Skip the Emscripten frontend |
| `OFFLINE_BUILD` | `OFF` | Adjust dependency fetching for sandboxed builds (Flatpak, Yocto) |
| `BUILD_WEBPAGE` | `OFF` | Build the marketing webpage module |
| `FETCH_YAML_CPP` / `FETCH_RAPIDFUZZ` / `FETCH_SPDLOG` / `FETCH_EFSW` | `ON` | Fetch vs. use system version |
| `WEBKIT_PATH` | "" | Use a custom WebKit build instead of system pkg-config |
| `TAR_ARCHIVE_HAS_XZ` | `ON` | Enable liblzma compression in the tar-archive library |

## Debugging

The backend is a normal native binary; debug it directly with gdb or lldb:

```bash
gdb --args build/clang_debug/bin/nui-sftp
```

Two caveats worth knowing about:

- Tell the debugger to ignore `SIGUSR1` and `SIGXCPU`, otherwise the WebKit
  and JS runtimes will repeatedly trip it. For gdb:

  ```
  handle SIGUSR1 nostop noprint
  handle SIGXCPU nostop noprint pass
  ```

- On some WebKitGTK driver setups the DMABUF renderer crashes the webview.
  Export `WEBKIT_DISABLE_DMABUF_RENDERER=1` before launching if you see GPU
  process failures.

Test binaries live under `build/clang_debug/test/` and can be launched the
same way.

## Tests

Test sources live next to the code they cover, under `*/test/`:

```
ssh/test/ssh
backend/test/backend
utility/test/utility
shared_data/test/shared_data
tar-archive/test/tar_archive
```

They are enabled only when `NUI_SFTP_ENABLE_TESTING=ON`. Build and run them
explicitly, or use ctest:

```bash
# Build every test target in one go.
cmake --build build/clang_debug --target \
  nui-tests nui-sftp-backend-test nui-sftp-utility-test \
  nui-sftp-shared-data-test nui-sftp-ssh-test

# Run the whole suite.
ctest --test-dir build/clang_debug --output-on-failure

# Or run an individual binary.
./build/clang_debug/test/nui-sftp-utility-test
```

Tests must place transient files under the build tree (typically `programDirectory / "temp"`),
not `/tmp` or `std::filesystem::temp_directory_path()`. This keeps runs isolated
and reproducible across machines.

## Code style

- C++ source is formatted by clang-format using [.clang-format](.clang-format)
  (LLVM-derived, 120-column, 4-space indent, braces on their own line). Run it
  manually if your editor does not format on save:

  ```bash
  clang-format -i path/to/file.cpp
  ```

- I recommend clangd as the language server. The repository's own
  per-directory configuration is not committed (`.clangd` is gitignored), but
  generating `compile_commands.json` via `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`
  (already set in the build invocations above) is enough for most setups. A
  minimal local `.clangd` that points at the debug build is:

  ```yaml
  CompileFlags:
    CompilationDatabase: "build/clang_debug"
    Add:
      - "-std=c++23"
  ```

  Frontend translation units compile through Emscripten and need a separate
  block matching paths under `frontend/`, `nui-file-explorer/`, and
  `dependencies/Nui/nui/include/nui/frontend/`. Point that block at
  `build/clang_debug/module_nui-sftp` instead.
- Include order: project and library headers first, then system headers, then
  the C++ standard library.
- Naming: spell types out (`Implementation`, not `Impl`; `Channel`, not `Ch`).
  Short names are tolerated for hot-path members like `impl_`. Trailing `_` is
  reserved for `private`/`protected` members; public struct fields have no
  suffix. Strongly-typed IDs from `ids/` (`Ids::ChannelId`, `Ids::SessionId`,
  `Ids::OperationId`) replace bare `std::string` for identifiers.
- Doc comments use the `/** @brief ... @param ... */` block form, placed above
  the member they document. Do not write trailing comments and do not align
  doc comments across adjacent members.
- Default locals to `const` where they are not reassigned. Suffix unsigned
  integer literals with `u`. Avoid the `k`-prefix convention.
- Keep regular comments terse. Drop banner and "what does this do" comments;
  comment only when the *why* is not obvious from the code.

## Frontend (Nui) notes

The frontend uses Nui's C++ DSEL for the DOM. A few points worth knowing if
you are touching it:

- Empty children must be expressed with `Nui::nil()`. A default-constructed
  `Nui::ElementRenderer{}` is undefined behaviour.
- Do not call `eval()` from C++. Put support JS under `static/source/`, expose
  it on `globalThis`, and call it from C++ via `Nui::val::global(...)`.
- `Observed<T>::modifyNow()` syncs the UI immediately. Component setters
  should not sync; assign the `Observed` (or call `.modify()` for full-range
  invalidation) and let the caller batch.
- Themes use a `--brightness-mixin` variable: `black` on dark themes, `white`
  on light. Several rules mix it in at 80%, so flipping it across themes
  affects surface tones.

## Third-party licenses

The build runs [scripts/check_licenses.py](scripts/check_licenses.py) as part
of the main target. It fails the build if a CMake-declared dependency is
missing from [licenses/third_party.spdx.json](licenses/), if any referenced
license text file is empty, or if a placeholder slipped through. When adding a
new dependency, update the manifest and add its license file before pushing.

A standalone licenses tarball is produced in CI by
[scripts/build_licenses_bundle.py](scripts/build_licenses_bundle.py) and
attached to tagged releases.

## Continuous integration

Two workflows under [.github/workflows/](.github/workflows/) run on every PR
into `main` and on `v*` tag pushes:

- [archlinux.yml](.github/workflows/archlinux.yml) builds a Release config
  inside an `archlinux:base-devel` container, runs `ctest`, packages the
  frontend tarball and licenses tarball, and on tag pushes uploads them to the
  GitHub release and to MEGA S4.
- [windows.yml](.github/workflows/windows.yml) builds the same Release config
  under MSYS2 `clang64` and runs `scripts/deploy.sh` to produce the Windows
  artifacts.

PRs should pass both workflows. If you add a new CMake option or dependency,
update both workflows if it changes the build surface.

## Submitting a pull request

1. Fork the repository and create a feature branch from `main`.
2. Keep commits focused; prefer small, reviewable changes over large rewrites.
3. Run a debug build with tests enabled locally and make sure `ctest
   --output-on-failure` passes from `build/clang_debug`.
4. Format any C++ changes with clang-format.
5. Open the PR against `main`. Describe the user-visible behaviour, the
   motivation, and anything reviewers should pay particular attention to.
6. CI will run the Arch and Windows workflows; address any failures before
   requesting review.

## VS Code starter configuration

`.vscode/` is gitignored. If you want a VS Code setup similar to mine, drop
the snippets below into `.vscode/tasks.json` and `.vscode/launch.json`. They
cover the configure/build/test cycle and a single debug launch; extend them as
needed.

### `.vscode/tasks.json`

```json
{
    "version": "2.0.0",
    "options": { "cwd": "${workspaceFolder}" },
    "tasks": [
        {
            "label": "configure_debug",
            "type": "shell",
            "command": "cmake",
            "args": [
                "-S", ".", "-B", "build/clang_debug", "-G", "Ninja",
                "-DCMAKE_BUILD_TYPE=Debug",
                "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++",
                "-DCMAKE_CXX_STANDARD=23",
                "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                "-DNUI_FETCH_TRAITS=OFF", "-DNUI_FETCH_ROAR=OFF",
                "-DNUI_SFTP_ENABLE_TESTING=ON"
            ],
            "problemMatcher": []
        },
        {
            "label": "build_debug",
            "type": "shell",
            "dependsOn": "configure_debug",
            "command": "cmake",
            "args": ["--build", "build/clang_debug"],
            "group": { "kind": "build", "isDefault": true }
        },
        {
            "label": "ctest",
            "type": "shell",
            "dependsOn": "build_debug",
            "command": "ctest",
            "args": ["--test-dir", "build/clang_debug", "--output-on-failure"],
            "group": "test"
        }
    ]
}
```

### `.vscode/launch.json`

```json
{
    "version": "0.2.0",
    "configurations": [
        {
            "type": "gdb",
            "request": "launch",
            "name": "Build & Debug",
            "target": "${workspaceFolder}/build/clang_debug/bin/nui-sftp",
            "cwd": "${workspaceFolder}",
            "preLaunchTask": "build_debug",
            "debugger_args": [
                "--init-eval-command", "handle SIGUSR1 nostop noprint",
                "--init-eval-command", "handle SIGXCPU nostop noprint pass"
            ]
        }
    ]
}
```

(The `gdb` debug type comes from the
[Native Debug](https://marketplace.visualstudio.com/items?itemName=webfreak.debug)
extension. Substitute your preferred extension's schema if you use a different
one.)

## License

By contributing, you agree that your contributions are licensed under the same
terms as the rest of the project. See [LICENSE](LICENSE).
