#include <language_check/source_files.hpp>
#include <build_environment.hpp>

#include <yaml-cpp/yaml.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace
{
    using KeyPath = std::vector<std::string>;

    struct SourceLocation
    {
        std::string file;
        int line{};
    };

    // Maps each key path to all source locations that reference it.
    using KeyLocations = std::map<KeyPath, std::vector<SourceLocation>>;

    void collectYamlPaths(const YAML::Node& node, const KeyPath& currentPath, std::set<KeyPath>& paths)
    {
        if (!node.IsMap())
            return;
        for (const auto& kv : node)
        {
            KeyPath newPath = currentPath;
            newPath.push_back(kv.first.as<std::string>());
            paths.insert(newPath);
            collectYamlPaths(kv.second, newPath, paths);
        }
    }

    int lineOf(const std::string& content, std::ptrdiff_t offset)
    {
        return 1 + static_cast<int>(std::count(content.begin(), content.begin() + offset, '\n'));
    }

    // Extracts all language->get(...) and language->getObserved(...) calls with source locations.
    // Handles up to 4 string literal arguments and multi-line calls.
    KeyLocations extractLanguageCalls(const std::vector<std::filesystem::path>& files)
    {
        // Custom delimiter R"re(...)re" avoids early termination — the pattern contains
        // the sequence )" (from "([^"]+)") which would end a plain R"(...)" literal.
        static const std::regex pattern(
            R"re(language\s*->\s*get(?:Observed)?\s*\(\s*"([^"]+)"(?:\s*,\s*"([^"]+)")?(?:\s*,\s*"([^"]+)")?(?:\s*,\s*"([^"]+)")?\s*\))re",
            std::regex::ECMAScript
        );

        KeyLocations result;

        for (const auto& filePath : files)
        {
            std::ifstream file(filePath, std::ios::binary);
            if (!file.is_open())
                continue;

            const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            const std::string fileStr = filePath.string();

            auto begin = std::sregex_iterator(content.begin(), content.end(), pattern);
            const auto end = std::sregex_iterator{};

            for (auto it = begin; it != end; ++it)
            {
                const auto& match = *it;
                KeyPath keyPath;
                for (int i = 1; i <= 4; ++i)
                {
                    if (match[i].matched)
                        keyPath.push_back(match[i].str());
                }
                if (keyPath.empty())
                    continue;

                const int line = lineOf(content, match.position());
                result[keyPath].push_back({fileStr, line});
            }
        }

        return result;
    }

    bool isLeafNode(const YAML::Node& root, const KeyPath& keyPath)
    {
        YAML::Node current = root;
        for (const auto& part : keyPath)
        {
            if (!current.IsMap())
                return false;
            current = current[part];
        }
        return current.IsDefined() && !current.IsMap();
    }
}

int main()
{
    const auto sourceDir = std::filesystem::path(SOURCE_DIR);
    const auto languagesDir = sourceDir / "static" / "assets" / "languages";

    const auto files = deepScanSources(sourceDir);
    const auto codeKeys = extractLanguageCalls(files);

    if (codeKeys.empty())
    {
        nlohmann::json err;
        err["error"] = "No language keys found. Check that the source directory is correct.";
        std::cout << err.dump(2) << "\n";
        return 1;
    }

    if (!std::filesystem::exists(languagesDir))
    {
        nlohmann::json err;
        err["error"] = "Languages directory not found: " + languagesDir.string();
        std::cout << err.dump(2) << "\n";
        return 1;
    }

    nlohmann::json output;
    output["source_dir"] = sourceDir.string();
    output["languages_dir"] = languagesDir.string();
    output["languages"] = nlohmann::json::array();

    bool anyErrors = false;

    for (const auto& entry : std::filesystem::directory_iterator(languagesDir))
    {
        if (entry.path().extension() != ".yaml")
            continue;

        const auto& yamlPath = entry.path();

        YAML::Node yaml;
        try
        {
            yaml = YAML::LoadFile(yamlPath.string());
        }
        catch (const YAML::Exception& e)
        {
            nlohmann::json langEntry;
            langEntry["file"] = yamlPath.string();
            langEntry["locale"] = yamlPath.stem().string();
            langEntry["error"] = e.what();
            output["languages"].push_back(std::move(langEntry));
            anyErrors = true;
            continue;
        }

        std::set<KeyPath> yamlPaths;
        collectYamlPaths(yaml, {}, yamlPaths);

        // Missing: key referenced in code but absent from this YAML file
        nlohmann::json missingArray = nlohmann::json::array();
        for (const auto& [keyPath, locations] : codeKeys)
        {
            if (yamlPaths.find(keyPath) != yamlPaths.end())
                continue;

            nlohmann::json keyEntry;
            keyEntry["key"] = keyPath;

            nlohmann::json usages = nlohmann::json::array();
            for (const auto& loc : locations)
            {
                nlohmann::json usage;
                usage["file"] = loc.file;
                usage["line"] = loc.line;
                usages.push_back(std::move(usage));
            }
            keyEntry["usages"] = std::move(usages);
            missingArray.push_back(std::move(keyEntry));
            anyErrors = true;
        }

        // Unused: leaf key in YAML not referenced by any code
        nlohmann::json unusedArray = nlohmann::json::array();
        for (const auto& yamlKey : yamlPaths)
        {
            if (!isLeafNode(yaml, yamlKey))
                continue;
            if (codeKeys.find(yamlKey) == codeKeys.end())
                unusedArray.push_back(yamlKey);
        }

        nlohmann::json langEntry;
        langEntry["file"] = yamlPath.string();
        langEntry["locale"] = yamlPath.stem().string();
        langEntry["missing"] = std::move(missingArray);
        langEntry["unused"] = std::move(unusedArray);
        output["languages"].push_back(std::move(langEntry));
    }

    std::cout << output.dump(2) << "\n";
#ifdef __linux__
    std::ofstream outFile("/tmp/language_check_output.json");
    outFile << output.dump(2) << "\n";
    outFile.close();
#endif
    return anyErrors ? 1 : 0;
}
