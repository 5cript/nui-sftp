#include <frontend/file_explorer/side_model.hpp>

void SideModel::engine(std::unique_ptr<FileEngine> fileEngine)
{
    fileEngine_ = std::move(fileEngine);
}

FileEngine* SideModel::engine()
{
    return fileEngine_.get();
}

void SideModel::operationQueue(OperationQueue* operationQueue)
{
    operationQueue_ = operationQueue;
}
OperationQueue* SideModel::operationQueue()
{
    return operationQueue_;
}

bool SideModel::isComplete() const
{
    return fileEngine_ != nullptr && operationQueue_ != nullptr;
}

void SideModel::setItemUpdateFunction(std::function<void(bool)> doUpdate)
{
    refreshCallback_ = std::move(doUpdate);
}