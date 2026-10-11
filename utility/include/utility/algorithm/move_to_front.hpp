#pragma once

#include <algorithm>
#include <cstddef>
#include <iterator>

namespace Utility::Algorithm
{
    /**
     * @brief Moves the element at the index to the front, keeping the order of the other elements.
     *
     * @param container The container to reorder.
     * @param index The index of the element to move. An index out of range changes nothing.
     */
    template <typename ContainerT>
    void moveToFront(ContainerT& container, std::size_t index)
    {
        if (index >= std::size(container))
            return;
        const auto begin = std::begin(container);
        const auto element = std::next(begin, static_cast<std::ptrdiff_t>(index));
        std::rotate(begin, element, std::next(element));
    }
}
