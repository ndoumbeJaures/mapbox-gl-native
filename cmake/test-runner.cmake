add_executable(mbgl-test-runner
    test-runner/main.cpp
    test-runner/parser.cpp
    test-runner/runner.cpp
    platform/default/src/mbgl/util/timer.cpp
)

target_include_directories(mbgl-test-runner
    PRIVATE src
    PRIVATE platform/default/include
    PRIVATE test-runner
)

target_link_libraries(mbgl-test-runner PRIVATE
    mbgl-core
    mbgl-filesource
    mbgl-loop-uv
    rapidjson
    sqlite
    args
    pixelmatch-cpp
    expected
    -pthread
    -lstdc++fs
)

add_definitions(-DTEST_RUNNER_ROOT_PATH="${CMAKE_SOURCE_DIR}")