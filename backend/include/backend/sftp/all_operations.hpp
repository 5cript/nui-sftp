#include <backend/sftp/operation.hpp>
#include <backend/sftp/download_operation.hpp>
#include <backend/sftp/upload_operation.hpp>
#include <backend/sftp/scan_operation.hpp>
#include <backend/sftp/bulk_download_operation.hpp>

template <typename FunctionT>
auto Operation::visit(FunctionT&& func) const
{
    using enum SharedData::OperationType;
    switch (type())
    {
        case Download:
        {
            return func(static_cast<DownloadOperation const&>(*this));
        }
        case Upload:
        {
            return func(static_cast<UploadOperation const&>(*this));
        }
        case Scan:
        {
            return func(static_cast<ScanOperation const&>(*this));
        }
        case BulkDownload:
        {
            return func(static_cast<BulkDownloadOperation const&>(*this));
        }
        default:
        {
            Log::error("Operation: Unknown operation type: {}", static_cast<int>(type()));
            return func(std::nullopt);
        }
    }
}