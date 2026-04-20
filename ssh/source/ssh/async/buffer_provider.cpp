#include <ssh/async/buffer_provider.hpp>

#include <utility>

namespace SecureShell
{
    BufferLease::BufferLease(
        std::weak_ptr<IBufferProvider> owner,
        BufferCategory category,
        std::size_t slotIndex,
        char* data,
        std::size_t size,
        std::size_t capacity
    ) noexcept
        : owner_{std::move(owner)}
        , category_{category}
        , slotIndex_{slotIndex}
        , data_{data}
        , size_{size}
        , capacity_{capacity}
    {}

    BufferLease::BufferLease(BufferLease&& other) noexcept
        : owner_{std::move(other.owner_)}
        , category_{other.category_}
        , slotIndex_{other.slotIndex_}
        , data_{other.data_}
        , size_{other.size_}
        , capacity_{other.capacity_}
    {
        other.data_ = nullptr;
        other.size_ = 0;
        other.capacity_ = 0;
    }

    BufferLease& BufferLease::operator=(BufferLease&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            owner_ = std::move(other.owner_);
            category_ = other.category_;
            slotIndex_ = other.slotIndex_;
            data_ = other.data_;
            size_ = other.size_;
            capacity_ = other.capacity_;
            other.data_ = nullptr;
            other.size_ = 0;
            other.capacity_ = 0;
        }
        return *this;
    }

    BufferLease::~BufferLease()
    {
        reset();
    }

    void BufferLease::reset() noexcept
    {
        if (data_ == nullptr)
            return;
        if (auto provider = owner_.lock())
            provider->release(category_, slotIndex_);
        data_ = nullptr;
        size_ = 0;
        capacity_ = 0;
    }

    BufferLease IBufferProvider::makeLease(
        BufferCategory category,
        std::size_t slotIndex,
        char* data,
        std::size_t size,
        std::size_t capacity
    )
    {
        return BufferLease{weak_from_this(), category, slotIndex, data, size, capacity};
    }
}
