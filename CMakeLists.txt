cmake_minimum_required(VERSION 3.16)
project(TrueGoal CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)

find_package(Threads REQUIRED)

option(TRUE_GOAL_MOCK_GPIO "Simulate GPIO for testing off the BeagleY-AI" OFF)

if(TRUE_GOAL_MOCK_GPIO)
    set(GPIO_SOURCE tests/gpio_mock.cpp)
else()
    set(GPIO_SOURCE src/gpio.cpp)
endif()

add_library(true_goal_core STATIC
    src/buzzer.cpp
    src/camera.cpp
    src/controller.cpp
    src/detector.cpp
    src/encoder.cpp
    src/goalcam.cpp
    src/leds.cpp
    src/network.cpp
    src/replay.cpp
    ${GPIO_SOURCE}
)
target_include_directories(true_goal_core PUBLIC include)
target_link_libraries(true_goal_core PUBLIC Threads::Threads)

add_executable(true_goal
    src/main.cpp
    src/sensors.cpp
)
target_link_libraries(true_goal PRIVATE true_goal_core)

if(TRUE_GOAL_MOCK_GPIO)
    enable_testing()
    add_executable(true_goal_tests tests/test_true_goal.cpp)
    target_include_directories(true_goal_tests PRIVATE tests)
    target_link_libraries(true_goal_tests PRIVATE true_goal_core)
    add_test(NAME true_goal_tests COMMAND true_goal_tests)
endif()
