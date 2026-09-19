cmake_minimum_required(VERSION 3.16)

project(stackchan_firmware_tests LANGUAGES CXX)

enable_testing()

add_executable(motion_math_test
    motion_math_test.cpp
    ../main/stackchan/motion/motion_math.cpp
)

target_include_directories(motion_math_test PRIVATE
    ../main
)

target_compile_features(motion_math_test PRIVATE cxx_std_17)

add_test(NAME motion_math_test COMMAND $<TARGET_FILE:motion_math_test>)

add_executable(gemini_turn_state_test gemini_turn_state_test.cpp)
target_include_directories(gemini_turn_state_test PRIVATE ../main)
target_compile_features(gemini_turn_state_test PRIVATE cxx_std_17)
add_test(NAME gemini_turn_state_test COMMAND $<TARGET_FILE:gemini_turn_state_test>)

add_executable(codex_pet_animation_test codex_pet_animation_test.cpp)
target_include_directories(codex_pet_animation_test PRIVATE ../main)
target_compile_features(codex_pet_animation_test PRIVATE cxx_std_17)
add_test(NAME codex_pet_animation_test COMMAND $<TARGET_FILE:codex_pet_animation_test>)

set(SMOOTH_UI_TOOLKIT_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../components/smooth_ui_toolkit/src")
if(EXISTS "${SMOOTH_UI_TOOLKIT_SRC}/smooth_ui_toolkit.hpp")
    file(GLOB_RECURSE SMOOTH_UI_CORE_SOURCES CONFIGURE_DEPENDS "${SMOOTH_UI_TOOLKIT_SRC}/core/*.cpp")
    add_executable(motion_pause_test
        motion_pause_test.cpp
        ../main/stackchan/motion/motion.cpp
        ../main/stackchan/motion/servo.cpp
        ../main/stackchan/motion/motion_math.cpp
        ${SMOOTH_UI_CORE_SOURCES}
    )
    target_include_directories(motion_pause_test PRIVATE stubs ../main "${SMOOTH_UI_TOOLKIT_SRC}")
    target_compile_features(motion_pause_test PRIVATE cxx_std_17)
    add_test(NAME motion_pause_test COMMAND $<TARGET_FILE:motion_pause_test>)
else()
    message(STATUS "Skipping motion_pause_test: run fetch_repos.py to fetch Smooth UI Toolkit")
endif()
