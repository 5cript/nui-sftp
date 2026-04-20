#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <mutex>
#include <tuple>
#include <type_traits>
#include <vector>

namespace SecureShell
{
    /**
     * @brief Size categories for pooled SFTP transfer buffers.
     */
    enum class BufferCategory
    {
        Tiny,
        Small,
        Medium,
        Large,
        Max,
    };

    /**
     * @brief Byte size associated with each category.
     */
    constexpr std::size_t bufferCategorySize(BufferCategory category) noexcept
    {
        switch (category)
        {
            case BufferCategory::Tiny:
                return 1ull * 1024ull;
            case BufferCategory::Small:
                return 4ull * 1024ull;
            case BufferCategory::Medium:
                return 16ull * 1024ull;
            case BufferCategory::Large:
                return 64ull * 1024ull;
            case BufferCategory::Max:
                return 256ull * 1024ull;
        }
        return 0u;
    }

    /**
     * @brief Storage kind per category. Tiny/Small are inline std::array; larger categories use std::vector.
     */
    template <BufferCategory Category>
    struct CategoryStorage;

    template <>
    struct CategoryStorage<BufferCategory::Tiny>
    {
        using type = std::array<char, bufferCategorySize(BufferCategory::Tiny)>;
    };
    template <>
    struct CategoryStorage<BufferCategory::Small>
    {
        using type = std::array<char, bufferCategorySize(BufferCategory::Small)>;
    };
    template <>
    struct CategoryStorage<BufferCategory::Medium>
    {
        using type = std::vector<char>;
    };
    template <>
    struct CategoryStorage<BufferCategory::Large>
    {
        using type = std::vector<char>;
    };
    template <>
    struct CategoryStorage<BufferCategory::Max>
    {
        using type = std::vector<char>;
    };

    /**
     * @brief Variadic slot descriptor used to configure a BufferProvider.
     */
    template <BufferCategory Category, std::size_t Count>
    struct CategorySlots
    {
        static constexpr BufferCategory category = Category;
        static constexpr std::size_t count = Count;
        static_assert(Count >= 1u, "each category needs at least one slot");
    };

    /**
     * @brief A single bucket of pre-allocated buffers for one category.
     */
    template <BufferCategory Category, std::size_t Count>
    struct TypedBucket
    {
        using Storage = typename CategoryStorage<Category>::type;

        std::array<Storage, Count> slots{};
        std::array<bool, Count> taken{};

        TypedBucket()
        {
            if constexpr (std::is_same_v<Storage, std::vector<char>>)
            {
                for (auto& slot : slots)
                    slot.resize(bufferCategorySize(Category));
            }
        }
    };

    class IBufferProvider;

    /**
     * @brief Move-only RAII handle for a pooled buffer slot. Returns the slot to the provider on destruction.
     */
    class BufferLease
    {
      public:
        BufferLease() noexcept = default;
        ~BufferLease();

        BufferLease(BufferLease&& other) noexcept;
        BufferLease& operator=(BufferLease&& other) noexcept;
        BufferLease(BufferLease const&) = delete;
        BufferLease& operator=(BufferLease const&) = delete;

        char* data() noexcept
        {
            return data_;
        }
        char const* data() const noexcept
        {
            return data_;
        }
        std::size_t size() const noexcept
        {
            return size_;
        }
        std::size_t capacity() const noexcept
        {
            return capacity_;
        }
        BufferCategory category() const noexcept
        {
            return category_;
        }
        bool empty() const noexcept
        {
            return data_ == nullptr;
        }
        explicit operator bool() const noexcept
        {
            return !empty();
        }

        void reset() noexcept;

      private:
        friend class IBufferProvider;

        BufferLease(
            std::weak_ptr<IBufferProvider> owner,
            BufferCategory category,
            std::size_t slotIndex,
            char* data,
            std::size_t size,
            std::size_t capacity
        ) noexcept;

        std::weak_ptr<IBufferProvider> owner_{};
        BufferCategory category_{BufferCategory::Tiny};
        std::size_t slotIndex_{0};
        char* data_{nullptr};
        std::size_t size_{0};
        std::size_t capacity_{0};
    };

    /**
     * @brief Type-erased interface so callers do not see the BufferProvider's template parameters.
     */
    class IBufferProvider : public std::enable_shared_from_this<IBufferProvider>
    {
      public:
        virtual ~IBufferProvider() = default;

        /**
         * @brief Lease a buffer sized for the caller's working set.
         * @param sizeHint Expected working size (e.g. file size).
         * @return An empty lease if the chosen category and its one-smaller fallback are both exhausted.
         */
        virtual BufferLease lease(std::size_t sizeHint) = 0;

        /**
         * @brief Lease a buffer and additionally clamp the reported usable size by a server-negotiated limit.
         *        The underlying slot still has its full category capacity.
         */
        virtual BufferLease leaseForTransfer(std::size_t sizeHint, std::size_t serverLimit) = 0;

      protected:
        friend class BufferLease;

        virtual void release(BufferCategory category, std::size_t slotIndex) noexcept = 0;

        /**
         * @brief Helper for derived classes to construct leases.
         */
        BufferLease makeLease(
            BufferCategory category,
            std::size_t slotIndex,
            char* data,
            std::size_t size,
            std::size_t capacity
        );
    };

    namespace Detail
    {
        template <typename... Slots>
        constexpr bool allCategoriesPresentOnce() noexcept
        {
            constexpr std::array<BufferCategory, 5> required{
                BufferCategory::Tiny,
                BufferCategory::Small,
                BufferCategory::Medium,
                BufferCategory::Large,
                BufferCategory::Max,
            };
            constexpr std::array<BufferCategory, sizeof...(Slots)> provided{Slots::category...};

            for (auto req : required)
            {
                std::size_t count = 0;
                for (auto prov : provided)
                {
                    if (prov == req)
                        ++count;
                }
                if (count != 1)
                    return false;
            }
            return true;
        }

        template <BufferCategory Category, typename... Slots>
        constexpr std::size_t findBucketIndex() noexcept
        {
            constexpr std::array<BufferCategory, sizeof...(Slots)> cats{Slots::category...};
            for (std::size_t index = 0; index < cats.size(); ++index)
            {
                if (cats[index] == Category)
                    return index;
            }
            return static_cast<std::size_t>(-1);
        }

        constexpr BufferCategory oneCategorySmaller(BufferCategory category) noexcept
        {
            switch (category)
            {
                case BufferCategory::Max:
                    return BufferCategory::Large;
                case BufferCategory::Large:
                    return BufferCategory::Medium;
                case BufferCategory::Medium:
                    return BufferCategory::Small;
                case BufferCategory::Small:
                    return BufferCategory::Tiny;
                case BufferCategory::Tiny:
                    return BufferCategory::Tiny;
            }
            return BufferCategory::Tiny;
        }
    }

    /**
     * @brief Pre-allocated, fixed-capacity buffer pool with compile-time bucket configuration.
     *
     * @tparam MaxCategoryThresholdBytes Files strictly smaller than this are not eligible for the Max bucket.
     * @tparam Slots One CategorySlots<> per BufferCategory value; all five categories must be present exactly once.
     */
    template <std::size_t MaxCategoryThresholdBytes, typename... Slots>
    class BufferProvider final : public IBufferProvider
    {
        static_assert(sizeof...(Slots) == 5u, "BufferProvider must be configured with exactly 5 CategorySlots");
        static_assert(
            Detail::allCategoriesPresentOnce<Slots...>(),
            "BufferProvider must include each BufferCategory exactly once"
        );

      public:
        /**
         * @brief Factory — BufferProvider requires shared-ownership (enable_shared_from_this).
         */
        static std::shared_ptr<BufferProvider> create()
        {
            return std::shared_ptr<BufferProvider>{new BufferProvider{}};
        }

        static constexpr std::size_t maxCategoryThresholdBytes = MaxCategoryThresholdBytes;

        BufferLease lease(std::size_t sizeHint) override
        {
            const auto category = pickCategory(sizeHint);
            const auto reported = bufferCategorySize(category);
            return tryLeaseWithFallback(category, reported);
        }

        BufferLease leaseForTransfer(std::size_t sizeHint, std::size_t serverLimit) override
        {
            const auto category = pickCategory(sizeHint);
            const auto reported = std::min(bufferCategorySize(category), serverLimit);
            return tryLeaseWithFallback(category, reported);
        }

      private:
        BufferProvider() = default;

        template <BufferCategory Category>
        auto& bucket() noexcept
        {
            constexpr auto index = Detail::findBucketIndex<Category, Slots...>();
            return std::get<index>(buckets_);
        }

        BufferCategory pickCategory(std::size_t sizeHint) const noexcept
        {
            const BufferCategory cap = (sizeHint >= MaxCategoryThresholdBytes) ? BufferCategory::Max
                                                                               : BufferCategory::Large;

            constexpr std::array<BufferCategory, 5> ordered{
                BufferCategory::Tiny,
                BufferCategory::Small,
                BufferCategory::Medium,
                BufferCategory::Large,
                BufferCategory::Max,
            };

            for (auto candidate : ordered)
            {
                if (static_cast<int>(candidate) > static_cast<int>(cap))
                    break;
                if (bufferCategorySize(candidate) >= sizeHint)
                    return candidate;
            }
            return cap;
        }

        BufferLease tryLeaseWithFallback(BufferCategory requested, std::size_t reportedSize)
        {
            BufferLease lease = tryLeaseCategory(requested, reportedSize);
            if (lease)
                return lease;

            if (requested == BufferCategory::Tiny)
                return lease;

            const auto fallback = Detail::oneCategorySmaller(requested);
            const auto fallbackReported = std::min(reportedSize, bufferCategorySize(fallback));
            return tryLeaseCategory(fallback, fallbackReported);
        }

        BufferLease tryLeaseCategory(BufferCategory category, std::size_t reportedSize)
        {
            switch (category)
            {
                case BufferCategory::Tiny:
                    return tryLeaseImpl<BufferCategory::Tiny>(reportedSize);
                case BufferCategory::Small:
                    return tryLeaseImpl<BufferCategory::Small>(reportedSize);
                case BufferCategory::Medium:
                    return tryLeaseImpl<BufferCategory::Medium>(reportedSize);
                case BufferCategory::Large:
                    return tryLeaseImpl<BufferCategory::Large>(reportedSize);
                case BufferCategory::Max:
                    return tryLeaseImpl<BufferCategory::Max>(reportedSize);
            }
            return {};
        }

        template <BufferCategory Category>
        BufferLease tryLeaseImpl(std::size_t reportedSize)
        {
            std::lock_guard lock{mutex_};
            auto& b = bucket<Category>();
            for (std::size_t index = 0; index < b.taken.size(); ++index)
            {
                if (!b.taken[index])
                {
                    b.taken[index] = true;
                    char* const data = b.slots[index].data();
                    return makeLease(Category, index, data, reportedSize, bufferCategorySize(Category));
                }
            }
            return {};
        }

        void release(BufferCategory category, std::size_t slotIndex) noexcept override
        {
            std::lock_guard lock{mutex_};
            switch (category)
            {
                case BufferCategory::Tiny:
                    bucket<BufferCategory::Tiny>().taken[slotIndex] = false;
                    return;
                case BufferCategory::Small:
                    bucket<BufferCategory::Small>().taken[slotIndex] = false;
                    return;
                case BufferCategory::Medium:
                    bucket<BufferCategory::Medium>().taken[slotIndex] = false;
                    return;
                case BufferCategory::Large:
                    bucket<BufferCategory::Large>().taken[slotIndex] = false;
                    return;
                case BufferCategory::Max:
                    bucket<BufferCategory::Max>().taken[slotIndex] = false;
                    return;
            }
        }

        std::tuple<TypedBucket<Slots::category, Slots::count>...> buckets_{};
        mutable std::mutex mutex_{};
    };

    /**
     * @brief Default pool: 8/8/8 small-to-medium slots, 4/4 large slots, Max gated at 1 MiB.
     */
    using DefaultBufferProvider = BufferProvider<
        1ull * 1024ull * 1024ull,
        CategorySlots<BufferCategory::Tiny, 8>,
        CategorySlots<BufferCategory::Small, 8>,
        CategorySlots<BufferCategory::Medium, 8>,
        CategorySlots<BufferCategory::Large, 4>,
        CategorySlots<BufferCategory::Max, 4>>;
}
