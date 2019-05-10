#include <mbgl/util/logging.hpp>
#include <mbgl/util/io.hpp>
#include <mbgl/util/rapidjson.hpp>

#include <args.hxx>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "parser.hpp"
#include "metadata.hpp"

namespace {

std::string removeURLArguments(const std::string &url) {
    std::string::size_type index = url.find('?');
    if (index != std::string::npos) {
        return url.substr(0, index);
    }
    return url;
}

std::string prependFileScheme(const std::string &url) {
    static const std::string fileScheme("file://");
    return fileScheme + url;
}

mbgl::optional<std::string> getVendorPath(const mbgl::filesystem::path& url, const std::regex& regex, bool glyphsPath = false) {
    static const mbgl::filesystem::path vendorPath(std::string(TEST_RUNNER_ROOT_PATH) + "/vendor/");

    mbgl::filesystem::path file = std::regex_replace(url.string(), regex, vendorPath.string());
    if (mbgl::filesystem::exists(file.parent_path())) {
        return removeURLArguments(file.string());
    }

    if (glyphsPath && mbgl::filesystem::exists(file.parent_path().parent_path())) {
        return removeURLArguments(file.string());
    }

    return {};
}

mbgl::optional<std::string> getIntegrationPath(const mbgl::filesystem::path& url, const std::string& parent, const std::regex& regex, bool glyphsPath = false) {
    static const mbgl::filesystem::path integrationPath(std::string(TEST_RUNNER_ROOT_PATH) + "/mapbox-gl-js/test/integration/");

    mbgl::filesystem::path file = std::regex_replace(url.string(), regex, integrationPath.string() + parent);
    if (mbgl::filesystem::exists(file.parent_path())) {
        return removeURLArguments(file.string());
    }

    if (glyphsPath && mbgl::filesystem::exists(file.parent_path().parent_path())) {
        return removeURLArguments(file.string());
    }

    return {};
}

mbgl::optional<std::string> localizeLocalURL(const std::string& url, bool glyphsPath = false) {
    static const std::regex regex { "local://" };
    if (auto vendorPath = getVendorPath(url, regex, glyphsPath)) {
        return vendorPath;
    } else {
        return getIntegrationPath(url, "", regex, glyphsPath);
    }
}

mbgl::optional<std::string> localizeHttpURL(const std::string& url) {
    static const std::regex regex { "http://localhost:2900" };
    if (auto vendorPath = getVendorPath(url, regex)) {
        return vendorPath;
    } else {
        return getIntegrationPath(url, "", regex);
    }
}

mbgl::optional<std::string> localizeMapboxSpriteURL(const std::string& url) {
    static const std::regex regex { "mapbox://" };
    return getIntegrationPath(url, "", regex);
}

mbgl::optional<std::string> localizeMapboxFontsURL(const std::string& url) {
    static const std::regex regex { "mapbox://fonts" };
    return getIntegrationPath(url, "glyphs/", regex, true);
}

mbgl::optional<std::string> localizeMapboxTilesURL(const std::string& url) {
    static const std::regex regex { "mapbox://" };
    if (auto vendorPath = getVendorPath(url, regex)) {
        return vendorPath;
    } else {
        return getIntegrationPath(url, "tiles/", regex);
    }
}

mbgl::optional<std::string> localizeMapboxTilesetURL(const std::string& url) {
    static const std::regex regex { "mapbox://" };
    return getIntegrationPath(url, "tilesets/", regex);
}

} // anonymous namespace

mbgl::optional<mbgl::JSDocument> readJson(const mbgl::filesystem::path& jsonPath) {
    auto maybeJSON = mbgl::util::readFile(jsonPath);
    if (!maybeJSON) {
        mbgl::Log::Error(mbgl::Event::General, "Unable to open file %s", jsonPath.c_str());
        return {};
    }

    mbgl::JSDocument document;
    document.Parse<0>(*maybeJSON);
    if (document.HasParseError()) {
        mbgl::Log::Error(mbgl::Event::ParseStyle, mbgl::formatJSONParseError(document).c_str());
        return {};
    }

    return { std::move(document) };
}

std::string serializeJsonValue(const mbgl::JSValue& value) {
    rapidjson::StringBuffer buffer;
    buffer.Clear();
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    value.Accept(writer);
    return buffer.GetString();
}

ArgumentsTuple parseArguments(int argc, char** argv) {
    args::ArgumentParser argumentParser("Mapbox GL Test Runner");

    args::HelpFlag helpFlag(argumentParser, "help", "Display this help menu", { 'h', "help" });

    args::Flag recycleMapFlag(argumentParser, "recycle map", "Toggle reusing the map object",
                              { 'r', "recycle-map" });
    args::Flag shuffleFlag(argumentParser, "shuffle", "Toggle shuffling the tests order",
                           { 's', "shuffle" });
    args::ValueFlag<uint32_t> seedValue(argumentParser, "seed", "Shuffle seed (default: random)",
                                        { "seed" });
    args::ValueFlag<std::string> testPathValue(argumentParser, "rootPath", "Test root rootPath",
                                               { 'p', "rootPath" });
    args::PositionalList<std::string> testNameValues(argumentParser, "URL", "Test name(s)");

    try {
        argumentParser.ParseCLI(argc, argv);
    } catch (const args::Help&) {
        std::ostringstream stream;
        stream << argumentParser;
        mbgl::Log::Info(mbgl::Event::General, stream.str());
        exit(0);
    } catch (const args::ParseError& e) {
        std::ostringstream stream;
        stream << argumentParser;
        mbgl::Log::Info(mbgl::Event::General, stream.str());
        mbgl::Log::Error(mbgl::Event::General, e.what());
        exit(1);
    } catch (const args::ValidationError& e) {
        std::ostringstream stream;
        stream << argumentParser;
        mbgl::Log::Info(mbgl::Event::General, stream.str());
        mbgl::Log::Error(mbgl::Event::General, e.what());
        exit(2);
    }

    const std::string testDefaultPath =
        std::string(TEST_RUNNER_ROOT_PATH).append("/mapbox-gl-js/test/integration/render-tests");

    return ArgumentsTuple {
        recycleMapFlag ? args::get(recycleMapFlag) : false,
        shuffleFlag ? args::get(shuffleFlag) : false, seedValue ? args::get(seedValue) : 1u,
        testPathValue ? args::get(testPathValue) : testDefaultPath, args::get(testNameValues)
    };
}

std::vector<std::pair<std::string, std::string>> parseIgnores() {
    std::vector<std::pair<std::string, std::string>> ignores;

    auto path = mbgl::filesystem::path(TEST_RUNNER_ROOT_PATH).append("platform/node/test/ignores.json");

    auto maybeIgnores = readJson(path.string());
    if (maybeIgnores) {
        for (const auto& property : maybeIgnores->GetObject()) {
            const std::string ignore = { property.name.GetString(),
                                         property.name.GetStringLength() };
            const std::string reason = { property.value.GetString(),
                                         property.value.GetStringLength() };
            ignores.push_back(std::make_pair(ignore, reason));
        }
    }

    return ignores;
}

mbgl::optional<TestMetadata> parseTestMetadata(const mbgl::filesystem::path& path) {
    TestMetadata metadata;
    metadata.path = path;

    auto maybeJson = readJson(path.string());
    if (!maybeJson) {
        mbgl::Log::Error(mbgl::Event::ParseStyle, "Unable to parse: %s", path.c_str());
        return {};
    }

    metadata.document = std::move(*maybeJson);
    localizeStyleURLs(metadata.document, metadata.document);

    if (!metadata.document.HasMember("metadata")) {
        mbgl::Log::Warning(mbgl::Event::ParseStyle, "Style has no 'metadata': %s",
                           path.c_str());
        return metadata;
    }

    const mbgl::JSValue& metadataValue = metadata.document["metadata"];
    if (!metadataValue.HasMember("test")) {
        mbgl::Log::Warning(mbgl::Event::ParseStyle, "Style has no 'metadata.test': %s",
                           path.c_str());
        return metadata;
    }

    const mbgl::JSValue& testValue = metadataValue["test"];

    if (testValue.HasMember("width")) {
        assert(testValue["width"].IsNumber());
        metadata.size.width = testValue["width"].GetInt();
    }

    if (testValue.HasMember("height")) {
        assert(testValue["height"].IsNumber());
        metadata.size.height = testValue["height"].GetInt();
    }

    if (testValue.HasMember("pixelRatio")) {
        assert(testValue["pixelRatio"].IsNumber());
        metadata.pixelRatio = testValue["pixelRatio"].GetDouble();
    }

    if (testValue.HasMember("allowed")) {
        assert(testValue["allowed"].IsNumber());
        metadata.allowed = testValue["allowed"].GetDouble();
    }

    if (testValue.HasMember("description")) {
        assert(testValue["description"].IsString());
        metadata.description = std::string{ testValue["description"].GetString(),
                                                testValue["description"].GetStringLength() };
    }

    if (testValue.HasMember("mapMode")) {
        assert(testValue["mapMode"].IsString());
        metadata.mapMode = testValue["mapMode"].GetString() == std::string("tile") ? mbgl::MapMode::Tile : mbgl::MapMode::Static;
    }

    // Test operations handled in runner.cpp.

    if (testValue.HasMember("debug")) {
        metadata.debug |= mbgl::MapDebugOptions::TileBorders;
    }

    if (testValue.HasMember("collisionDebug")) {
        metadata.debug |= mbgl::MapDebugOptions::Collision;
    }

    if (testValue.HasMember("showOverdrawInspector")) {
        metadata.debug |= mbgl::MapDebugOptions::Overdraw;
    }

    if (testValue.HasMember("crossSourceCollisions")) {
        assert(testValue["crossSourceCollisions"].IsBool());
        metadata.crossSourceCollisions = testValue["crossSourceCollisions"].GetBool();
    }

    if (testValue.HasMember("axonometric")) {
        assert(testValue["axonometric"].IsBool());
        metadata.axonometric = testValue["axonometric"].GetBool();
    }

    if (testValue.HasMember("skew")) {
        assert(testValue["skew"].IsArray());
        metadata.xSkew = testValue["skew"][0].GetDouble();
        metadata.ySkew = testValue["skew"][1].GetDouble();
    }

    // TODO: fadeDuration
    if (testValue.HasMember("fadeDuration")) {
        return {};
    }

    // TODO: addFakeCanvas
    if (testValue.HasMember("addFakeCanvas")) {
        return {};
    }

    return metadata;
}

std::string localizeURL(const std::string& url) {
    static const std::regex regex { "local://" };
    if (auto vendorPath = getVendorPath(url, regex)) {
        return *vendorPath;
    } else {
        return getIntegrationPath(url, "", regex).value_or(url);
    }
}

void localizeSourceURLs(mbgl::JSValue& root, mbgl::JSDocument& document) {
    if (root.HasMember("urls") && root["urls"].IsArray()) {
        for (auto& urlValue : root["urls"].GetArray()) {
            const std::string path = prependFileScheme(localizeMapboxTilesetURL(urlValue.GetString())
                .value_or(localizeLocalURL(urlValue.GetString())
                    .value_or(urlValue.GetString())));
            urlValue.Set<std::string>(path, document.GetAllocator());
        }
    }

    if (root.HasMember("url")) {
        mbgl::JSValue& urlValue = root["url"];
        const std::string path = prependFileScheme(localizeMapboxTilesetURL(urlValue.GetString())
            .value_or(localizeLocalURL(urlValue.GetString())
                .value_or(urlValue.GetString())));
        urlValue.Set<std::string>(path, document.GetAllocator());

        if ((strcmp(root["type"].GetString(), "image") != 0) && (strcmp(root["type"].GetString(), "video") != 0)) {
            const auto tilesetPath = std::string(urlValue.GetString()).erase(0u, 7u); // remove "file://"
            if (auto maybeTileset = readJson(tilesetPath)) {
                assert(maybeTileset->HasMember("tiles"));
                root.AddMember("tiles", (*maybeTileset)["tiles"], document.GetAllocator());
                root.RemoveMember("url");
            }
        }
    }

    if (root.HasMember("tiles")) {
        mbgl::JSValue& tilesValue = root["tiles"];
        assert(tilesValue.IsArray());
        for (auto& tileValue : tilesValue.GetArray()) {
            const std::string path = prependFileScheme(localizeMapboxTilesURL(tileValue.GetString())
                .value_or(localizeLocalURL(tileValue.GetString())
                    .value_or(localizeHttpURL(tileValue.GetString())
                        .value_or(tileValue.GetString()))));
            tileValue.Set<std::string>(path, document.GetAllocator());
        }
    }

    if (root.HasMember("data") && root["data"].IsString()) {
        mbgl::JSValue& dataValue = root["data"];
        const std::string path = prependFileScheme(localizeLocalURL(dataValue.GetString())
            .value_or(dataValue.GetString()));
        dataValue.Set<std::string>(path, document.GetAllocator());
    }
}

void localizeStyleURLs(mbgl::JSValue& root, mbgl::JSDocument& document) {
    if (root.HasMember("sources")) {
        mbgl::JSValue& sourcesValue = root["sources"];
        for (auto& sourceProperty : sourcesValue.GetObject()) {
            localizeSourceURLs(sourceProperty.value, document);
        }
    }

    if (root.HasMember("glyphs")) {
        mbgl::JSValue& glyphsValue = root["glyphs"];
        const std::string path = prependFileScheme(localizeMapboxFontsURL(glyphsValue.GetString())
            .value_or(localizeLocalURL(glyphsValue.GetString(), true)
                .value_or(glyphsValue.GetString())));
        glyphsValue.Set<std::string>(path, document.GetAllocator());
    }

    if (root.HasMember("sprite")) {
        mbgl::JSValue& spriteValue = root["sprite"];
        const std::string path = prependFileScheme(localizeMapboxSpriteURL(spriteValue.GetString())
            .value_or(localizeLocalURL(spriteValue.GetString())
                .value_or(spriteValue.GetString())));
        spriteValue.Set<std::string>(path, document.GetAllocator());
    }
}
