#pragma once

#include <nui-file-explorer/texts.hpp>
#include <nui-file-explorer/error.hpp>

#include <string>

/**
 * @brief The texts of the file explorer in the current language.
 */
NuiFileExplorer::Texts fileExplorerTexts();

/**
 * @brief The message for an error the file explorer reports, in the current language.
 */
std::string fileExplorerErrorText(NuiFileExplorer::Error const& error);
