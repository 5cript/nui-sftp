#pragma once

#include <nui/event_system/observed_value.hpp>
#include <log/log.hpp>

#include <algorithm>
#include <deque>
#include <stdexcept>

template <typename KeyT, typename ValueT, template <typename, typename> typename MapT>
class ObservedRandomAccessMap
{
  public:
    ObservedRandomAccessMap() = default;

    void insert(KeyT const& key, ValueT&& value)
    {
        if (keyToPointerMap_.find(key) != keyToPointerMap_.end())
            throw std::invalid_argument("Key already exists in ObservedRandomAccessMap");

        observedValues_.push_back(std::make_shared<ValueT>(std::move(value)));
        Log::info("Inserting key '{}' at index '{}'", key.value(), observedValues_.value().size() - 1);
        keyToPointerMap_[key] = observedValues_.value().back().get();
    }

    void pop_back()
    {
        if (observedValues_.value().empty())
            return;

        keyToPointerMap_.erase(observedValues_.value().back()->key());
        observedValues_.pop_back();
    }

    void pop_front()
    {
        if (observedValues_.value().empty())
            return;

        keyToPointerMap_.erase(observedValues_.value().front()->key());
        observedValues_.pop_front();
    }

    /**
     * @brief Extract the first @p count elements, returning ownership to the caller.
     *        Performs a single contiguous range-erase on the underlying Observed
     *        so Nui emits one block diff rather than @p count individual diffs.
     */
    std::vector<std::shared_ptr<ValueT>> extract_front_n(std::size_t count)
    {
        auto& deque = observedValues_.value();
        if (count > deque.size())
            count = deque.size();
        if (count == 0)
            return {};

        std::vector<std::shared_ptr<ValueT>> extracted;
        extracted.reserve(count);
        for (std::size_t idx = 0; idx < count; ++idx)
        {
            keyToPointerMap_.erase(deque[idx]->key());
            extracted.push_back(std::move(deque[idx]));
        }
        observedValues_.erase(observedValues_.begin(), observedValues_.begin() + count);
        return extracted;
    }

    ValueT* front()
    {
        if (observedValues_.value().empty())
            return nullptr;
        return observedValues_.value().front().get();
    }

    void erase(KeyT const& key)
    {
        auto mapIt = keyToPointerMap_.find(key);
        if (mapIt == keyToPointerMap_.end())
            throw std::out_of_range("Key not found in ObservedRandomAccessMap");

        ValueT* ptr = mapIt->second;
        keyToPointerMap_.erase(mapIt);

        auto& deque = observedValues_.value();
        auto dequeIt = std::find_if(
            deque.begin(),
            deque.end(),
            [ptr](std::shared_ptr<ValueT> const& uptr)
            {
                return uptr.get() == ptr;
            }
        );
        if (dequeIt == deque.end())
            throw std::logic_error("Pointer not found in deque during ObservedRandomAccessMap erase");

        observedValues_.erase(dequeIt);
    }

    Nui::Observed<std::deque<std::shared_ptr<ValueT>>>& observedValues()
    {
        return observedValues_;
    }

    ValueT* at(KeyT const& key)
    {
        auto it = keyToPointerMap_.find(key);
        if (it == keyToPointerMap_.end())
            return nullptr;
        return it->second;
    }

    /**
     * @brief Return a shared_ptr to the element for @p key, or nullptr if absent.
     *        Lets callers share ownership with other containers without copying.
     */
    std::shared_ptr<ValueT> shared(KeyT const& key)
    {
        auto mapIt = keyToPointerMap_.find(key);
        if (mapIt == keyToPointerMap_.end())
            return nullptr;

        ValueT* ptr = mapIt->second;
        for (auto const& entry : observedValues_.value())
        {
            if (entry.get() == ptr)
                return entry;
        }
        return nullptr;
    }

    auto find(KeyT const& key)
    {
        auto mapIt = keyToPointerMap_.find(key);
        if (mapIt == keyToPointerMap_.end())
            return observedValues_.value().end();

        ValueT* ptr = mapIt->second;
        return std::find_if(
            observedValues_.value().begin(),
            observedValues_.value().end(),
            [ptr](std::shared_ptr<ValueT> const& uptr)
            {
                return uptr.get() == ptr;
            }
        );
    }

    auto end()
    {
        return observedValues_.value().end();
    }

    template <typename FunctionT>
    void modify(KeyT const& key, FunctionT const& modifier)
    {
        auto it = keyToPointerMap_.find(key);
        if (it == keyToPointerMap_.end())
            throw std::out_of_range("Key not found in ObservedRandomAccessMap");

        modifier(*it->second);
    }

    bool empty() const
    {
        return observedValues_.value().empty();
    }

    void clear()
    {
        observedValues_.clear();
        keyToPointerMap_.clear();
    }

  private:
    Nui::Observed<std::deque<std::shared_ptr<ValueT>>> observedValues_;
    MapT<KeyT, ValueT*> keyToPointerMap_;
};
