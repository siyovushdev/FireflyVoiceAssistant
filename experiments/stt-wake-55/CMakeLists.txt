cmake_minimum_required(VERSION 3.16)
project(FireflySttWakeEvaluation LANGUAGES CXX)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    message(FATAL_ERROR "Evaluation requires Linux/aarch64")
endif()
get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(SHERPA_RUNTIME "${REPO}/build/stt/sherpa-onnx-v1.13.7-linux-aarch64-shared-cpu")
# Deliberately compile only the unchanged speech adapters and framing. There is
# no production CMake change, core library, VoiceProcessor, CommandEngine or RKLLM.
add_executable(stt-wake-replay src/SttWakeReplay.cpp
    "${REPO}/src/SherpaSpeech.cpp" "${REPO}/src/VoiceSegmenter.cpp")
target_compile_features(stt-wake-replay PRIVATE cxx_std_17)
set_target_properties(stt-wake-replay PROPERTIES CXX_EXTENSIONS OFF
    BUILD_RPATH "${SHERPA_RUNTIME}/lib")
target_compile_options(stt-wake-replay PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(stt-wake-replay PRIVATE "${REPO}/include" "${REPO}/build/stt/vad-ab")
find_package(Threads REQUIRED)
target_link_libraries(stt-wake-replay PRIVATE
    "${SHERPA_RUNTIME}/lib/libsherpa-onnx-c-api.so" Threads::Threads)
enable_testing()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME ExactWakePrefix COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/PrefixTests.py")
set_tests_properties(ExactWakePrefix PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
