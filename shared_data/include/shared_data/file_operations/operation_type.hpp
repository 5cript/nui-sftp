#pragma once

#include <shared_data/shared_data.hpp>
#include <utility/enum_string_convert.hpp>
#include <utility/describe.hpp>

namespace SharedData
{
    BOOST_DEFINE_ENUM_CLASS(OperationType, Scan, Download, BulkDownload, Upload, BulkUpload, Rename, Delete)
}