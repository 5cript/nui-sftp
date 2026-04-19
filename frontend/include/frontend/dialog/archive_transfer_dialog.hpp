#pragma once

#include <nui/frontend/element_renderer.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>

/**
 * @brief Compression codec choice exposed to the user in the Transfer-as-Archive
 * dialog. Intentionally decoupled from TarArchive::Compression because the frontend
 * (EMSCRIPTEN) build does not link against the backend-only tar-archive library;
 * the backend layer translates this enum when the transfer is actually enqueued.
 */
enum class ArchiveCodec
{
    None,
    Gzip,
    Bzip2,
    Zstd,
    Xz
};

/**
 * @brief The filename extension segment that accompanies a given codec. `.tar` is
 * always implied by the dialog and is never part of this string. For None the
 * suffix is empty (plain `.tar`).
 */
std::string archiveCodecExtension(ArchiveCodec codec);

/**
 * @brief Reverse mapping: file extension segment ("gz", "bz2", ...) → codec.
 * Falls back to None for the empty string.
 */
ArchiveCodec archiveCodecFromExtension(std::string const& extension);

/**
 * @brief Structured result handed to the caller when the dialog is confirmed.
 *
 * @c fileStem is the user-entered filename without any `.tar[.ext]` suffix; the
 * caller composes the full filename using @c archiveCodecExtension.
 * @c compressionLevel is a user-facing 1..9 value ("fastest"..."smallest") that
 * the backend will rescale onto the codec's native range.
 */
struct ArchiveTransferResult
{
    std::string fileStem;
    ArchiveCodec codec{ArchiveCodec::Gzip};
    int compressionLevel{5};
};

/**
 * @brief Modal dialog that collects an archive filename stem, a compression
 * codec (extension dropdown), and a compression level (1..9 slider) from the
 * user. Mirrors the pimpl pattern used by InputDialog.
 */
class ArchiveTransferDialog
{
  public:
    ArchiveTransferDialog(std::string id);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(ArchiveTransferDialog);

    /**
     * @brief Render hook — place this exactly once in the DOM tree near the
     * other top-level dialogs (see MainPage::render).
     */
    Nui::ElementRenderer operator()();

    struct OpenOptions
    {
        /** @brief Dialog header text (e.g. "Download as Archive"). */
        std::string headerText{};

        /** @brief Initial filename stem shown in the input (no extension). */
        std::string initialFileStem{};

        /** @brief Initial codec selection. */
        ArchiveCodec initialCodec{ArchiveCodec::Gzip};

        /** @brief Initial compression level (clamped to 1..9). */
        int initialCompressionLevel{5};

        /**
         * @brief Invoked exactly once when the dialog closes. Engaged when the
         * user confirmed; nullopt on cancel.
         */
        std::function<void(std::optional<ArchiveTransferResult> const&)> onConfirm;
    };

    /**
     * @brief Open the dialog. Any previous open() that has not yet fired its
     * onConfirm will have its callback replaced.
     */
    void open(OpenOptions const& options);

  private:
    Nui::ElementRenderer dialogBody();

    struct Implementation;
    std::unique_ptr<Implementation> impl_;
};
