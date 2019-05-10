#include <mbgl/util/logging.hpp>
#include <mbgl/util/optional.hpp>
#include <mbgl/util/filesystem.hpp>
#include <mbgl/util/run_loop.hpp>

#include "metadata.hpp"
#include "parser.hpp"
#include "runner.hpp"

#include <random>

#define ANSI_COLOR_RED        "\x1b[31m"
#define ANSI_COLOR_GREEN      "\x1b[32m"
#define ANSI_COLOR_YELLOW     "\x1b[33m"
#define ANSI_COLOR_BLUE       "\x1b[34m"
#define ANSI_COLOR_MAGENTA    "\x1b[35m"
#define ANSI_COLOR_CYAN       "\x1b[36m"
#define ANSI_COLOR_GRAY       "\x1b[37m"
#define ANSI_COLOR_LIGHT_GRAY "\x1b[90m"
#define ANSI_COLOR_RESET      "\x1b[0m"

int main(int argc, char** argv) {
    bool recycleMap;
    bool shuffle;
    uint32_t seedNumber;
    std::string testRootPath;
    std::vector<std::string> testNames;

    std::tie(recycleMap, shuffle, seedNumber, testRootPath, testNames) = parseArguments(argc, argv);

    const auto ignores = parseIgnores();

    // Traverse all test root path if no test has been specified.
    if (testNames.empty()) {
        testNames.push_back({});
    }

    // Recursively traverse through the test paths and collect test directories containing "style.json".
    std::vector<mbgl::filesystem::path> testPaths;
    for (const auto& testName : testNames) {
        const auto absolutePath = mbgl::filesystem::path(testRootPath) / mbgl::filesystem::path(testName);
        for (auto& testPath : mbgl::filesystem::recursive_directory_iterator(absolutePath)) {
            if (testPath.path().filename() == "style.json") {
                testPaths.push_back(testPath);
            }
        }
    }

    if (shuffle) {
        printf(ANSI_COLOR_YELLOW "Shuffle seed: %d" ANSI_COLOR_RESET "\n", seedNumber);

        std::seed_seq seed{ seedNumber };
        std::mt19937 shuffler(seed);
        std::shuffle(testPaths.begin(), testPaths.end(), shuffler);
    }

    mbgl::util::RunLoop runLoop;
    TestRunner runner;

    for (auto& testPath : testPaths) {
        mbgl::optional<TestMetadata> metadata = parseTestMetadata(testPath);
        if (!metadata) {
            continue;
        }

        if (!recycleMap) {
            runner.reset();
        }

        std::string testName = testPath.remove_filename().string();
        testName.erase(testName.find(testRootPath), testRootPath.length() + 1);
        testName = testName.substr(0, testName.length() - 1);

        bool shouldIgnore = false;
        std::string ignoreReason;

        const std::string ignoreName = "render-tests/" + testName;
        const auto it = std::find_if(ignores.cbegin(), ignores.cend(), [&ignoreName](auto pair) { return pair.first == ignoreName; });
        if (it != ignores.end()) {
            shouldIgnore = true;
            ignoreReason = it->second;
            if (ignoreReason.rfind("skip", 0) == 0) {
                printf(ANSI_COLOR_GRAY "* skipped %s (%s)" ANSI_COLOR_RESET "\n", testName.c_str(), ignoreReason.c_str());
                continue;
            }
        }

        double diff = runner.run(*metadata);
        bool passed = diff <= metadata->allowed;

        if (shouldIgnore) {
            if (passed) {
                printf(ANSI_COLOR_YELLOW "* ignore %s (%s)" ANSI_COLOR_RESET "\n", testName.c_str(), ignoreReason.c_str());
            } else {
                printf(ANSI_COLOR_LIGHT_GRAY "* ignore %s (%s)" ANSI_COLOR_RESET "\n", testName.c_str(), ignoreReason.c_str());
            }
        } else {
            if (passed) {
                printf(ANSI_COLOR_GREEN "* passed %s" ANSI_COLOR_RESET "\n", testName.c_str());
            } else {
                printf(ANSI_COLOR_RED "* failed %s" ANSI_COLOR_RESET "\n", testName.c_str());
            }
        }
    }

    return 0;
}
