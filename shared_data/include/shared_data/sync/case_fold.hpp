#pragma once

#include <string>
#include <string_view>

namespace SharedData::Sync
{
    /**
     * @brief The key under which names meet on a filesystem that folds case or normalization, like NTFS, APFS and
     *        HFS+: every letter is lowercased (ASCII, Latin-1, Latin Extended-A, Greek, Cyrillic, fullwidth Latin)
     *        and every precomposed letter of those scripts is decomposed into its base and combining marks, so an
     *        NFC and an NFD spelling of a name get the same key. Text that is not UTF-8 is its own key.
     */
    std::string caseFoldedName(std::string_view name);

    /**
     * @brief False when @p name is its own case folded key, which holds for most names and costs no allocation.
     */
    bool foldingChanges(std::string_view name);
}
