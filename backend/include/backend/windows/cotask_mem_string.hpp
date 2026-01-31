#pragma once

#include <nui/utility/utf.hpp>

#include <combaseapi.h>
#include <utility>
#include <string>

class CoTaskMemString
{
  public:
    explicit CoTaskMemString(wchar_t* ptr)
        : ptr_{ptr}
    {}
    ~CoTaskMemString()
    {
        if (ptr_ != nullptr)
            CoTaskMemFree(ptr_);
    }
    CoTaskMemString(CoTaskMemString const&) = delete;
    CoTaskMemString& operator=(CoTaskMemString const&) = delete;
    CoTaskMemString(CoTaskMemString&& other) noexcept
        : ptr_{std::exchange(other.ptr_, nullptr)}
    {}
    CoTaskMemString& operator=(CoTaskMemString&& other) noexcept
    {
        if (this != &other)
        {
            if (ptr_ != nullptr)
                CoTaskMemFree(ptr_);
            ptr_ = std::exchange(other.ptr_, nullptr);
        }
        return *this;
    }

    wchar_t** recepticle()
    {
        return &ptr_;
    }

    wchar_t* get() const
    {
        return ptr_;
    }

    operator wchar_t*() const
    {
        return ptr_;
    }

    std::wstring toStandardString() const
    {
        return std::wstring{ptr_};
    }

    std::string toUtf8String() const
    {
        return Nui::utf16ToUtf8<std::wstring, std::string>(toStandardString());
    }

  private:
    wchar_t* ptr_;
};