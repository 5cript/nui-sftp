#pragma once

#include <frontend/settings/atomic_setting/setting.hpp>

template <bool Disengageable, typename ValueType>
void assignIfValid(
    typename Setting<Disengageable, ValueType>::OutfacingValueType& toSet,
    Setting<Disengageable, ValueType> const& setting
)
{
    if (setting.valueIsValid())
        toSet = setting.value();
}