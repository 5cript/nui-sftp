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
#include <sstream>
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

    struct LanguageCalls
    {
        // Key paths that are fully spelled out with literals.
        KeyLocations keys;
        // Leading literal parts of calls whose remaining parts are computed at runtime.
        KeyLocations dynamicPrefixes;
    };

    // Splits the arguments of a call at top level commas. `position` points behind the opening parenthesis and is
    // left behind the closing one.
    std::vector<std::string> splitArguments(const std::string& content, std::size_t& position)
    {
        std::vector<std::string> arguments;
        std::string current;
        int depth = 0;
        for (; position < content.size(); ++position)
        {
            const char character = content[position];
            if (character == '"' || character == '\'')
            {
                const auto literalStart = position;
                for (++position; position < content.size() && content[position] != character; ++position)
                {
                    if (content[position] == '\\')
                        ++position;
                }
                current.append(content, literalStart, position - literalStart + 1);
                continue;
            }
            if (character == '(' || character == '[' || character == '{')
                ++depth;
            else if (character == ')' || character == ']' || character == '}')
            {
                if (depth == 0)
                {
                    ++position;
                    arguments.push_back(std::move(current));
                    return arguments;
                }
                --depth;
            }
            else if (character == ',' && depth == 0)
            {
                arguments.push_back(std::move(current));
                current.clear();
                continue;
            }
            current.push_back(character);
        }
        return arguments;
    }

    // The key parts an argument can evaluate to: a literal, or both branches of a ternary over literals.
    // Empty if the argument is computed.
    std::vector<std::string> argumentAlternatives(const std::string& argument)
    {
        static const std::regex literal(R"re(^\s*"([^"\\]+)"\s*$)re");
        static const std::regex ternary(R"re(^[^?"]*\?\s*"([^"\\]+)"\s*:\s*"([^"\\]+)"\s*$)re");

        std::smatch match;
        if (std::regex_match(argument, match, literal))
            return {match[1].str()};
        if (std::regex_match(argument, match, ternary))
            return {match[1].str(), match[2].str()};
        return {};
    }

    std::vector<KeyPath> expand(const std::vector<std::vector<std::string>>& alternativesPerPart)
    {
        std::vector<KeyPath> paths{{}};
        for (const auto& alternatives : alternativesPerPart)
        {
            std::vector<KeyPath> extended;
            for (const auto& path : paths)
            {
                for (const auto& alternative : alternatives)
                {
                    auto next = path;
                    next.push_back(alternative);
                    extended.push_back(std::move(next));
                }
            }
            paths = std::move(extended);
        }
        return paths;
    }

    // Extracts all language->get(...) and language->getObserved(...) calls with source locations, including
    // multi-line calls and ternaries choosing between literal keys. Also collects the dot separated message keys of
    // code without access to the language files, which all start with "backend.".
    LanguageCalls extractLanguageCalls(const std::vector<std::filesystem::path>& files)
    {
        static const std::regex callStart(R"re(language\s*->\s*get(?:Observed)?\s*\()re", std::regex::ECMAScript);
        static const std::regex messageKey(R"re("(backend(?:\.[A-Za-z0-9_]+)+)")re", std::regex::ECMAScript);

        LanguageCalls result;

        for (const auto& filePath : files)
        {
            std::ifstream file(filePath, std::ios::binary);
            if (!file.is_open())
                continue;

            const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            const std::string fileStr = filePath.string();

            auto begin = std::sregex_iterator(content.begin(), content.end(), callStart);
            const auto end = std::sregex_iterator{};

            for (auto it = begin; it != end; ++it)
            {
                const auto& match = *it;
                auto position = static_cast<std::size_t>(match.position() + match.length());
                const auto arguments = splitArguments(content, position);

                std::vector<std::vector<std::string>> alternativesPerPart;
                for (const auto& argument : arguments)
                {
                    auto alternatives = argumentAlternatives(argument);
                    if (alternatives.empty())
                        break;
                    alternativesPerPart.push_back(std::move(alternatives));
                }
                if (alternativesPerPart.empty())
                    continue;

                auto& target = alternativesPerPart.size() == arguments.size() ? result.keys : result.dynamicPrefixes;
                const SourceLocation location{fileStr, lineOf(content, match.position())};
                for (auto& keyPath : expand(alternativesPerPart))
                    target[std::move(keyPath)].push_back(location);
            }

            for (auto it = std::sregex_iterator(content.begin(), content.end(), messageKey); it != end; ++it)
            {
                KeyPath keyPath;
                std::stringstream parts{(*it)[1].str()};
                for (std::string part; std::getline(parts, part, '.');)
                    keyPath.push_back(part);
                result.keys[std::move(keyPath)].push_back({fileStr, lineOf(content, it->position())});
            }
        }

        return result;
    }

    bool hasDynamicPrefix(const KeyLocations& dynamicPrefixes, const KeyPath& keyPath)
    {
        return std::ranges::any_of(
            dynamicPrefixes,
            [&keyPath](const auto& entry)
            {
                const auto& prefix = entry.first;
                return prefix.size() < keyPath.size() && std::equal(prefix.begin(), prefix.end(), keyPath.begin());
            }
        );
    }

    nlohmann::json usagesToJson(const std::vector<SourceLocation>& locations)
    {
        nlohmann::json usages = nlohmann::json::array();
        for (const auto& location : locations)
            usages.push_back({{"file", location.file}, {"line", location.line}});
        return usages;
    }

    bool isLeafNode(const YAML::Node& root, const KeyPath& keyPath)
    {
        YAML::Node current = root;
        for (const auto& part : keyPath)
        {
            if (!current.IsMap())
                return false;
            // Node::operator= assigns through to the shared tree, reset() rebinds.
            current.reset(current[part]);
        }
        return current.IsDefined() && !current.IsMap();
    }
}

int main()
{
    const auto sourceDir = std::filesystem::path(SOURCE_DIR);
    const auto languagesDir = sourceDir / "static" / "assets" / "languages";

    const auto files = deepScanSources(sourceDir);
    const auto calls = extractLanguageCalls(files);
    const auto& codeKeys = calls.keys;

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
    output["dynamic"] = nlohmann::json::array();
    for (const auto& [prefix, locations] : calls.dynamicPrefixes)
        output["dynamic"].push_back({{"prefix", prefix}, {"usages", usagesToJson(locations)}});

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
            keyEntry["usages"] = usagesToJson(locations);
            missingArray.push_back(std::move(keyEntry));
            anyErrors = true;
        }

        // Unused: leaf key in YAML not referenced by any code, keys below a computed call count as referenced
        nlohmann::json unusedArray = nlohmann::json::array();
        for (const auto& yamlKey : yamlPaths)
        {
            if (!isLeafNode(yaml, yamlKey))
                continue;
            if (codeKeys.find(yamlKey) == codeKeys.end() && !hasDynamicPrefix(calls.dynamicPrefixes, yamlKey))
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
