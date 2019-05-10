#pragma once

#include <mbgl/util/filesystem.hpp>
#include <mbgl/util/rapidjson.hpp>

#include <tuple>
#include <regex>
#include <string>
#include <vector>

struct TestMetadata;

using ArgumentsTuple = std::tuple<bool, bool, uint32_t, std::string, std::vector<std::string>>;

mbgl::optional<mbgl::JSDocument> readJson(const mbgl::filesystem::path&);
std::string serializeJsonValue(const mbgl::JSValue&);

ArgumentsTuple parseArguments(int argc, char** argv);
std::vector<std::pair<std::string, std::string>> parseIgnores();
mbgl::optional<TestMetadata> parseTestMetadata(const mbgl::filesystem::path& path);

std::string localizeURL(const std::string& url);

void localizeSourceURLs(mbgl::JSValue& root, mbgl::JSDocument& document);
void localizeStyleURLs(mbgl::JSValue& root, mbgl::JSDocument& document);