#pragma once

#include <mbgl/gfx/headless_frontend.hpp>
#include <mbgl/map/map.hpp>

#include <memory>

struct TestMetadata;

class TestRunner {
public:
    TestRunner() = default;

    double run(TestMetadata&);
    void reset();

private:
    void runOperations(const std::string& key, TestMetadata&);
    double checkImage(mbgl::PremultipliedImage&& image, const std::string& base);

    struct Impl {
        Impl(const TestMetadata&);

        mbgl::HeadlessFrontend frontend;
        mbgl::Map map;
    };
    std::unordered_map<std::string, std::unique_ptr<Impl>> maps;
};