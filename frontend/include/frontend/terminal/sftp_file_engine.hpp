#include <frontend/terminal/file_engine.hpp>
#include <frontend/terminal/ssh_engine.hpp>
#include <nui/utility/move_detector.hpp>
#include <roar/detail/pimpl_special_functions.hpp>

class SftpFileEngine : public FileEngine
{
  public:
    SftpFileEngine(SshTerminalEngine* engine);
    ROAR_PIMPL_SPECIAL_FUNCTIONS(SftpFileEngine);

    void listDirectory(
        std::filesystem::path const& path,
        std::function<void(std::optional<std::vector<SharedData::DirectoryEntry>> const&, std::string const& info)>
            onComplete
    ) override;
    void dispose(std::function<void()> onComplete) override;
    void createDirectory(
        std::filesystem::path const& path,
        std::function<void(bool, std::string const& info)> onComplete
    ) override;
    void createFile(
        std::filesystem::path const& path,
        std::function<void(bool, std::string const& info)> onComplete
    ) override;

    std::optional<Ids::ChannelId> release();

    void addDownload(
        NuiFileExplorer::Item const& remotePath,
        NuiFileExplorer::Item const& localPath,
        std::function<void(std::optional<Ids::OperationId>, std::string const& info)> onOperationCreated,
        bool allowOverwrite,
        bool insertRefresh
    ) override;
    void addUpload(
        NuiFileExplorer::Item const& remotePath,
        NuiFileExplorer::Item const& localPath,
        std::function<void(std::optional<Ids::OperationId>, std::string const& info)> onOperationCreated,
        bool allowOverwrite,
        bool insertRefresh
    ) override;

    void remove(
        std::vector<NuiFileExplorer::Item> const& files,
        std::vector<NuiFileExplorer::Item> const& directories,
        std::function<void(bool, std::string const& info)> onComplete,
        std::function<void(
            std::vector<std::filesystem::path>, /* regular files & empty */
            std::vector<std::filesystem::path> /* non empties */
        )> onNonEmptyDirectoriesFound
    ) override;

    void removeOnQueueUnchecked(
        std::vector<std::filesystem::path> const& paths,
        bool recursive,
        std::function<void(std::optional<std::vector<Ids::OperationId>> const&, std::string const& info)> onComplete
    ) override;

    void rename(
        std::filesystem::path const& oldPath,
        std::filesystem::path const& newPath,
        std::function<void(bool, std::string const& info)> onComplete
    ) override;

    void stat(
        std::filesystem::path const& path,
        std::function<
            void(std::optional<std::pair<bool /*exists*/, SharedData::DirectoryEntry>> const&, std::string const& info)>
            onComplete
    ) override;

  private:
    void lazyOpen(std::function<void(std::optional<Ids::ChannelId> const&, std::string const& info)> const& onOpen);
    void performDelete(
        std::vector<NuiFileExplorer::Item> files,
        std::vector<std::filesystem::path> directoriesEmpty,
        std::function<void(bool, std::string const& info)> onComplete
    );

  private:
    struct Implementation;
    std::unique_ptr<Implementation> impl_;
    Nui::MoveDetector moveDetector_;
};