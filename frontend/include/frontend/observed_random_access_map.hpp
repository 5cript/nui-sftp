#pragma once

#include <nui/event_system/observed_value.hpp>
#include <log/log.hpp>

#include <deque>
#include <stdexcept>

template <typename KeyT, typename ValueT, template <typename, typename> typename MapT>
class ObservedRandomAccessMap
{
  public:
    ObservedRandomAccessMap() = default;

    void insert(KeyT const& key, ValueT&& value)
    {
        if (keyToIndexMap_.find(key) != keyToIndexMap_.end())
            throw std::invalid_argument("Key already exists in ObservedRandomAccessMap");

        observedValues_.push_back(std::make_unique<ValueT>(std::move(value)));
        Log::info("Inserting key '{}' at index '{}'", key.value(), observedValues_.value().size() - 1);
        keyToIndexMap_[key] = observedValues_.value().size() - 1;
    }

    void pop_back()
    {
        if (observedValues_.value().empty())
            return;

        keyToIndexMap_.erase(observedValues_.value().back()->key());
        observedValues_.pop_back();
    }

    void pop_front()
    {
        if (observedValues_.empty())
            return;
        observedValues_.pop_front();

        // TODO: be more clever here:
        rebuildIndexMap();
    }

    ValueT* front()
    {
        if (observedValues_.value().empty())
            return nullptr;
        return observedValues_.value().front().get();
    }

    void erase(KeyT const& key)
    {
        auto it = keyToIndexMap_.find(key);
        if (it == keyToIndexMap_.end())
            throw std::out_of_range("Key not found in ObservedRandomAccessMap");

        Log::info("Erasing key '{}' at index '{}'", key.value(), it->second);
        observedValues_.erase(observedValues_.begin() + it->second);

        // TODO: be more clever here:
        rebuildIndexMap();
    }

    Nui::Observed<std::deque<std::unique_ptr<ValueT>>>& observedValues()
    {
        return observedValues_;
    }

    ValueT* at(KeyT const& key)
    {
        auto it = keyToIndexMap_.find(key);
        if (it == keyToIndexMap_.end())
            return nullptr;

        return observedValues_.value().at(it->second).get();
    }

    auto find(KeyT const& key)
    {
        auto it = keyToIndexMap_.find(key);
        if (it == keyToIndexMap_.end())
            return observedValues_.value().end();

        return observedValues_.value().begin() + it->second;
    }

    auto end()
    {
        return observedValues_.value().end();
    }

    template <typename FunctionT>
    void modify(KeyT const& key, FunctionT const& modifier)
    {
        auto it = keyToIndexMap_.find(key);
        if (it == keyToIndexMap_.end())
            throw std::out_of_range("Key not found in ObservedRandomAccessMap");

        modifier(*observedValues_[it->second].get());
    }

    bool empty() const
    {
        return observedValues_.value().empty();
    }

  private:
    void rebuildIndexMap()
    {
        keyToIndexMap_.clear();
        std::size_t i = 0;
        for (auto const& value : observedValues_.value())
        {
            keyToIndexMap_.emplace(value->key(), i);
            ++i;
        }
    }

  private:
    Nui::Observed<std::deque<std::unique_ptr<ValueT>>> observedValues_;
    MapT<KeyT, std::size_t> keyToIndexMap_;
};