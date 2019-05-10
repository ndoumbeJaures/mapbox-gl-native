#include <mbgl/map/camera.hpp>
#include <mbgl/map/map_observer.hpp>
#include <mbgl/style/conversion/filter.hpp>
#include <mbgl/style/conversion/layer.hpp>
#include <mbgl/style/conversion/source.hpp>
#include <mbgl/style/image.hpp>
#include <mbgl/style/layer.hpp>
#include <mbgl/style/style.hpp>
#include <mbgl/style/rapidjson_conversion.hpp>
#include <mbgl/util/chrono.hpp>
#include <mbgl/util/io.hpp>
#include <mbgl/util/image.hpp>
#include <mbgl/util/logging.hpp>
#include <mbgl/util/run_loop.hpp>
#include <mbgl/util/string.hpp>
#include <mbgl/util/timer.hpp>

#include <mapbox/pixelmatch.hpp>

#include "metadata.hpp"
#include "parser.hpp"
#include "runner.hpp"

#include <algorithm>
#include <cassert>
#include <regex>

double TestRunner::checkImage(mbgl::PremultipliedImage&& actual, const std::string& base) {

#if !TEST_READ_ONLY
    if (getenv("UPDATE")) {
        mbgl::util::write_file(base + "/expected.png", mbgl::encodePNG(actual));
        return true;
    }
#endif

    mbgl::optional<std::string> maybeExpectedImage = mbgl::util::readFile(base + "/expected.png");
    if (!maybeExpectedImage) {
        mbgl::Log::Error(mbgl::Event::Setup, "Failed to load expected image %s", (base + "/expected.png").c_str());
        return false;
    }

    mbgl::PremultipliedImage expected = mbgl::decodeImage(*maybeExpectedImage);
    mbgl::PremultipliedImage diff { expected.size };

#if !TEST_READ_ONLY
    mbgl::util::write_file(base + "/actual.png", mbgl::encodePNG(actual));
#endif

    if (expected.size != actual.size) {
        mbgl::Log::Error(mbgl::Event::Setup, "Expected and actual image sizes differ");
        return false;
    }

    double pixels = mapbox::pixelmatch(actual.data.get(),
                                       expected.data.get(),
                                       expected.size.width,
                                       expected.size.height,
                                       diff.data.get(),
                                       0.13);

#if !TEST_READ_ONLY
    mbgl::util::write_file(base + "/diff.png", mbgl::encodePNG(diff));
#endif

    return pixels / (expected.size.width * expected.size.height);
}

void TestRunner::runOperations(const std::string& key, TestMetadata& metadata) {
    if (!metadata.document.HasMember("metadata")) {
        return;
    }

    if (!metadata.document["metadata"].HasMember("test")) {
        return;
    }

    if (!metadata.document["metadata"]["test"].HasMember("operations")) {
        return;
    }

    assert(metadata.document["metadata"]["test"]["operations"].IsArray());

    const auto& operationsArray = metadata.document["metadata"]["test"]["operations"].GetArray();
    if (operationsArray.Empty()) {
        return;
    }

    const auto& operationIt = operationsArray.Begin();
    assert(operationIt->IsArray());

    const auto& operationArray = operationIt->GetArray();
    assert(operationArray.Size() >= 1u);

    auto& frontend = maps[key]->frontend;
    auto& map = maps[key]->map;

    // wait
    if (strcmp(operationArray[0].GetString(), "wait") == 0) {
        frontend.render(map);

    // sleep
    } else if (strcmp(operationArray[0].GetString(), "sleep") == 0) {
        mbgl::util::Timer timer;
        bool sleeping = true;

        mbgl::Duration duration = mbgl::Seconds(5);
        if (operationArray.Size() >= 2u) {
            duration = mbgl::Milliseconds(operationArray[1].GetUint());
        }

        timer.start(duration, mbgl::Duration::zero(), [&]() {
            sleeping = false;
        });

        while (sleeping) {
            mbgl::util::RunLoop::Get()->runOnce();
        }

    // addImage | updateImage
    } else if (strcmp(operationArray[0].GetString(), "addImage") == 0 || strcmp(operationArray[0].GetString(), "updateImage") == 0) {
        assert(operationArray.Size() >= 3u);

        float pixelRatio = 1.0;
        bool sdf = false;

        if (operationArray.Size() == 4u) {
            assert(operationArray[3].IsObject());
            const auto& imageOptions = operationArray[3].GetObject();
            if (imageOptions.HasMember("pixelRatio")) {
                pixelRatio = imageOptions["pixelRatio"].GetDouble();
            }
            if (imageOptions.HasMember("sdf")) {
                sdf = imageOptions["sdf"].GetBool();
            }
        }

        std::string imageName = operationArray[1].GetString();
        imageName.erase(std::remove(imageName.begin(), imageName.end(), '"'), imageName.end());

        std::string imagePath = operationArray[2].GetString();
        imagePath.erase(std::remove(imagePath.begin(), imagePath.end(), '"'), imagePath.end());

        const mbgl::filesystem::path filePath(std::string(TEST_RUNNER_ROOT_PATH) + "/mapbox-gl-js/test/integration/" + imagePath);

        mbgl::optional<std::string> maybeImage = mbgl::util::readFile(filePath.string());
        if (!maybeImage) {
            mbgl::Log::Error(mbgl::Event::Setup, "Failed to load expected image %s", filePath.c_str());
            return;
        }

        map.getStyle().addImage(std::make_unique<mbgl::style::Image>(imageName, mbgl::decodeImage(*maybeImage), pixelRatio, sdf));

    // setStyle
    } else if (strcmp(operationArray[0].GetString(), "setStyle") == 0) {
        assert(operationArray.Size() >= 2u);
        if (operationArray[1].IsString()) {
            std::string stylePath = localizeURL(operationArray[1].GetString());
            if (auto maybeStyle = readJson(stylePath)) {
                localizeStyleURLs(*maybeStyle, *maybeStyle);
                map.getStyle().loadJSON(serializeJsonValue(*maybeStyle));
            }
        } else {
            localizeStyleURLs(operationArray[1], metadata.document);
            map.getStyle().loadJSON(serializeJsonValue(operationArray[1]));
        }

    // setCenter
    } else if (strcmp(operationArray[0].GetString(), "setCenter") == 0) {
        assert(operationArray.Size() >= 2u);
        assert(operationArray[1].IsArray());

        const auto& centerArray = operationArray[1].GetArray();
        assert(centerArray.Size() == 2u);

        map.jumpTo(mbgl::CameraOptions().withCenter(mbgl::LatLng(centerArray[1].GetDouble(), centerArray[0].GetDouble())));

    // setZoom
    } else if (strcmp(operationArray[0].GetString(), "setZoom") == 0) {
        assert(operationArray.Size() >= 2u);
        assert(operationArray[1].IsNumber());
        map.jumpTo(mbgl::CameraOptions().withZoom(operationArray[1].GetDouble()));

    // setBearing
    } else if (strcmp(operationArray[0].GetString(), "setBearing") == 0) {
        assert(operationArray.Size() >= 2u);
        assert(operationArray[1].IsNumber());
        map.jumpTo(mbgl::CameraOptions().withBearing(operationArray[1].GetDouble()));

    // setFilter
    } else if (strcmp(operationArray[0].GetString(), "setFilter") == 0) {
        assert(operationArray.Size() >= 3u);
        assert(operationArray[1].IsString());

        const std::string layerName { operationArray[1].GetString(), operationArray[1].GetStringLength() };

        mbgl::style::conversion::Error error;
        auto converted = mbgl::style::conversion::convert<mbgl::style::Filter>(operationArray[2], error);
        if (!converted) {
            mbgl::Log::Error(mbgl::Event::ParseStyle, "Unable to convert filter: %s", error.message.c_str());
        } else {
            auto layer = map.getStyle().getLayer(layerName);
            if (!layer) {
                mbgl::Log::Error(mbgl::Event::ParseStyle, "Layer not found: %s", layerName.c_str());
            } else {
                layer->setFilter(std::move(*converted));
            }
        }

    // setLayerZoomRange
    } else if (strcmp(operationArray[0].GetString(), "setLayerZoomRange") == 0) {
        assert(operationArray.Size() >= 4u);
        assert(operationArray[1].IsString());
        assert(operationArray[2].IsNumber());
        assert(operationArray[3].IsNumber());

        const std::string layerName { operationArray[1].GetString(), operationArray[1].GetStringLength() };
        auto layer = map.getStyle().getLayer(layerName);
        if (!layer) {
            mbgl::Log::Error(mbgl::Event::ParseStyle, "Layer not found: %s", layerName.c_str());
        } else {
            layer->setMinZoom(operationArray[2].GetFloat());
            layer->setMaxZoom(operationArray[3].GetFloat());
        }

    // addLayer
    } else if (strcmp(operationArray[0].GetString(), "addLayer") == 0) {
        assert(operationArray.Size() >= 2u);
        assert(operationArray[1].IsObject());

        mbgl::style::conversion::Error error;
        auto converted = mbgl::style::conversion::convert<std::unique_ptr<mbgl::style::Layer>>(operationArray[1], error);
        if (!converted) {
            mbgl::Log::Error(mbgl::Event::ParseStyle, "Unable to convert layer: %s", error.message.c_str());
        } else {
            map.getStyle().addLayer(std::move(*converted));
        }

    // removeLayer
    } else if (strcmp(operationArray[0].GetString(), "removeLayer") == 0) {
        assert(operationArray.Size() >= 2u);
        assert(operationArray[1].IsString());
        map.getStyle().removeLayer(operationArray[1].GetString());

    // addSource
    } else if (strcmp(operationArray[0].GetString(), "addSource") == 0) {
        assert(operationArray.Size() >= 3u);
        assert(operationArray[1].IsString());
        assert(operationArray[2].IsObject());

        localizeSourceURLs(operationArray[2], metadata.document);

        mbgl::style::conversion::Error error;
        auto converted = mbgl::style::conversion::convert<std::unique_ptr<mbgl::style::Source>>(operationArray[2], error, operationArray[1].GetString());
        if (!converted) {
            mbgl::Log::Error(mbgl::Event::ParseStyle, "Unable to convert source: %s", error.message.c_str());
        } else {
            map.getStyle().addSource(std::move(*converted));
        }

    // removeSource
    } else if (strcmp(operationArray[0].GetString(), "removeSource") == 0) {
        assert(operationArray.Size() >= 2u);
        assert(operationArray[1].IsString());
        map.getStyle().removeSource(operationArray[1].GetString());

    // setPaintProperty
    } else if (strcmp(operationArray[0].GetString(), "setPaintProperty") == 0) {
        assert(operationArray.Size() >= 4u);
        assert(operationArray[1].IsString());
        assert(operationArray[2].IsString());

        const std::string layerName { operationArray[1].GetString(), operationArray[1].GetStringLength() };
        const std::string propertyName { operationArray[2].GetString(), operationArray[2].GetStringLength() };

        auto layer = map.getStyle().getLayer(layerName);
        if (!layer) {
            mbgl::Log::Error(mbgl::Event::ParseStyle, "Layer not found: %s", layerName.c_str());
        } else {
            const mbgl::JSValue* propertyValue = &operationArray[3];
            layer->setPaintProperty(propertyName, propertyValue);
        }

    // setLayoutProperty
    } else if (strcmp(operationArray[0].GetString(), "setLayoutProperty") == 0) {
        assert(operationArray.Size() >= 4u);
        assert(operationArray[1].IsString());
        assert(operationArray[2].IsString());

        const std::string layerName { operationArray[1].GetString(), operationArray[1].GetStringLength() };
        const std::string propertyName { operationArray[2].GetString(), operationArray[2].GetStringLength() };

        auto layer = map.getStyle().getLayer(layerName);
        if (!layer) {
            mbgl::Log::Error(mbgl::Event::ParseStyle, "Layer not found: %s", layerName.c_str());
        } else {
            const mbgl::JSValue* propertyValue = &operationArray[3];
            layer->setLayoutProperty(propertyName, propertyValue);
        }

    } else {
        mbgl::Log::Error(mbgl::Event::Setup, "Unsupported operation %s", operationArray[0].GetString());
    }

    operationsArray.Erase(operationIt);
    runOperations(key, metadata);
}

TestRunner::Impl::Impl(const TestMetadata& metadata)
    : frontend(metadata.size, metadata.pixelRatio),
      map(frontend,
          mbgl::MapObserver::nullObserver(),
          mbgl::MapOptions()
              .withMapMode(metadata.mapMode)
              .withSize(metadata.size)
              .withPixelRatio(metadata.pixelRatio)
              .withCrossSourceCollisions(metadata.crossSourceCollisions),
          mbgl::ResourceOptions()) {}

double TestRunner::run(TestMetadata& metadata) {
    std::string key = mbgl::util::toString(uint32_t(metadata.mapMode))
        + "/" + mbgl::util::toString(metadata.pixelRatio)
        + "/" + mbgl::util::toString(uint32_t(metadata.crossSourceCollisions));

    if (maps.find(key) == maps.end()) {
        maps[key] = std::make_unique<TestRunner::Impl>(metadata);
    }

    auto& frontend = maps[key]->frontend;
    auto& map = maps[key]->map;

    frontend.setSize(metadata.size);
    map.setSize(metadata.size);

    map.setProjectionMode(mbgl::ProjectionMode().withAxonometric(metadata.axonometric).withXSkew(metadata.xSkew).withYSkew(metadata.ySkew));
    map.setDebug(metadata.debug);

    map.getStyle().loadJSON(serializeJsonValue(metadata.document));

    runOperations(key, metadata);

    mbgl::PremultipliedImage actual = frontend.render(map);

    assert(map.isFullyLoaded());

    return checkImage(std::move(actual), metadata.path.remove_filename().string());
}

void TestRunner::reset() {
    maps.clear();
}