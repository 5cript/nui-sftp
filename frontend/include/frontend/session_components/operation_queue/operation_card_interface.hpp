#pragma once

#include <shared_data/file_operations/transfer_progress.hpp>
#include <shared_data/file_operations/bulk_progress.hpp>
#include <shared_data/file_operations/scan_progress.hpp>
#include <shared_data/file_operations/operation_added.hpp>
#include <shared_data/file_operations/operation_type.hpp>
#include <shared_data/file_operations/operation_error_type.hpp>
#include <shared_data/file_operations/operation_error.hpp>
#include <shared_data/file_operations/operation_state.hpp>
#include <shared_data/file_operations/operation_completed.hpp>
#include <shared_data/file_operations/operation_error.hpp>
#include <shared_data/is_paused.hpp>
#include <shared_data/error_or_success.hpp>

#include <nui/frontend/element_renderer.hpp>

#include <chrono>

class OperationCardInterface
{
  public:
    virtual ~OperationCardInterface() = default;

    virtual bool warrantsCancelConfirm() const = 0;
    virtual Nui::ElementRenderer body() const = 0;
    virtual void state(SharedData::OperationState newState) = 0;
    virtual SharedData::OperationState state() const = 0;
    virtual SharedData::OperationType type() const = 0;
    virtual bool isCompletedState() const = 0;
    virtual std::chrono::steady_clock::time_point completionTime() const = 0;
    virtual void completionTime(std::chrono::steady_clock::time_point time) = 0;
    virtual Nui::ElementRenderer operator()() const = 0;
    virtual void setError(SharedData::OperationError const& error) = 0;
};