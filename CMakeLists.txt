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
    src/sensors.cpp
    ${GPIO_SOURCE}
)
target_include_directories(true_goal_core PUBLIC include)
target_link_libraries(true_goal_core PUBLIC Threads::Threads)

add_executable(true_goal src/main.cpp)
target_link_libraries(true_goal PRIVATE true_goal_core)

if(TRUE_GOAL_MOCK_GPIO)
    target_compile_definitions(true_goal PRIVATE TRUE_GOAL_MOCK_GPIO)
    enable_testing()
    add_executable(true_goal_tests tests/test_true_goal.cpp)
    target_include_directories(true_goal_tests PRIVATE tests)
    target_link_libraries(true_goal_tests PRIVATE true_goal_core)
    add_test(NAME true_goal_tests COMMAND true_goal_tests)

    # goal_cam.py's evidence code, with a stand-in for the YOLO model.
    find_program(PYTHON3 python3)
    if(PYTHON3)
        add_test(NAME goal_cam_evidence COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/tests/test_goal_cam.py)
    endif()
endif()
