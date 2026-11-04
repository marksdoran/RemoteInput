# Builds the RemoteInput prototype with GCC or Clang, to check that the code is portable and to run it under
# the sanitizers. RemoteInput.sln is the build to use on Windows; nothing there depends on this file.
#
#   cmake -S . -B build -DREMOTE_INPUT_SANITIZE=thread && cmake --build build && ctest --test-dir build

cmake_minimum_required(VERSION 3.20)
project(RemoteInput LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(REMOTE_INPUT_SANITIZE "" CACHE STRING "Sanitizers to build with, e.g. address,undefined or thread")
option(REMOTE_INPUT_NO_EXCEPTIONS "Build with C++ exceptions disabled" OFF)

find_package(Threads REQUIRED)

# Settings shared by every target, third-party code included.
add_library(remote_input_build_options INTERFACE)
target_link_libraries(remote_input_build_options INTERFACE Threads::Threads)
if(REMOTE_INPUT_SANITIZE)
    target_compile_options(remote_input_build_options INTERFACE
        -fsanitize=${REMOTE_INPUT_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
    target_link_options(remote_input_build_options INTERFACE -fsanitize=${REMOTE_INPUT_SANITIZE})
    if(REMOTE_INPUT_SANITIZE MATCHES "thread" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # ASIO uses std::atomic_thread_fence, which ThreadSanitizer does not model; GCC warns about it.
        target_compile_options(remote_input_build_options INTERFACE -Wno-tsan)
    endif()
endif()
if(REMOTE_INPUT_NO_EXCEPTIONS)
    # Standalone ASIO does not detect -fno-exceptions by itself.
    target_compile_options(remote_input_build_options INTERFACE -fno-exceptions)
    target_compile_definitions(remote_input_build_options INTERFACE ASIO_NO_EXCEPTIONS)
endif()

# Warnings for our own code only.
add_library(remote_input_warnings INTERFACE)
target_compile_options(remote_input_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wnon-virtual-dtor -Wold-style-cast
    -Woverloaded-virtual -Wcast-qual -Wformat=2 -Werror)

# ASIO and the stand-in game headers are found the same way the game finds them: through the folder that
# contains ExtLibs. They are marked as system headers so that their warnings do not count as ours.
add_library(remote_input_external_headers INTERFACE)
target_include_directories(remote_input_external_headers SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/ExtLibs/asio/include
    ${CMAKE_CURRENT_SOURCE_DIR}/EngineStubs)
if(REMOTE_INPUT_NO_EXCEPTIONS)
    # Without exceptions, ASIO reports failures through a function that the program has to define.
    target_compile_options(remote_input_external_headers INTERFACE
        -include ${CMAKE_CURRENT_SOURCE_DIR}/Tests/AsioNoExceptions.h)
endif()

# The component itself: the code that moves into the game.
add_library(remote_input STATIC
    Source/RemoteInputAccessPolicy.cpp
    Source/RemoteInputHostResolver.cpp
    Source/RemoteInputLineProtocol.cpp
    Source/RemoteInputListener.cpp
    Source/RemoteInputLog.cpp
    Source/RemoteInputQueue.cpp
    Source/RemoteInputRateLimiter.cpp
    Source/RemoteInputServer.cpp
    Source/RemoteInputSession.cpp
    Source/RemoteInputSettings.cpp)
target_include_directories(remote_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Source)
target_link_libraries(remote_input
    PUBLIC remote_input_build_options remote_input_external_headers
    PRIVATE remote_input_warnings)

# The game-side glue, built against stand-ins for the game functions it calls.
add_library(remote_input_game STATIC
    Game/RemoteInputGame.cpp
    EngineStubs/GameStubs.cpp)
target_include_directories(remote_input_game PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/Game
    ${CMAKE_CURRENT_SOURCE_DIR}/EngineStubs)
target_link_libraries(remote_input_game PUBLIC remote_input PRIVATE remote_input_warnings)

add_library(gtest STATIC
    ExtLibs/googletest/googletest/src/gtest-all.cc
    ExtLibs/googletest/googletest/src/gtest_main.cc)
target_include_directories(gtest
    SYSTEM PUBLIC ExtLibs/googletest/googletest/include
    PRIVATE ExtLibs/googletest/googletest)
target_link_libraries(gtest PUBLIC remote_input_build_options)

add_executable(remote_input_tests
    Tests/RemoteInputAccessPolicyTests.cpp
    Tests/RemoteInputGameTests.cpp
    Tests/RemoteInputHostResolverTests.cpp
    Tests/RemoteInputLineProtocolTests.cpp
    Tests/RemoteInputLogTests.cpp
    Tests/RemoteInputQueueTests.cpp
    Tests/RemoteInputRateLimiterTests.cpp
    Tests/RemoteInputServerTests.cpp
    Tests/RemoteInputSettingsTests.cpp
    Tests/TestLog.cpp)
target_link_libraries(remote_input_tests PRIVATE remote_input_game gtest remote_input_warnings)

add_executable(remote_input_test_server TestServer/TestServerMain.cpp)
target_link_libraries(remote_input_test_server PRIVATE remote_input_game remote_input_warnings)

add_executable(remote_input_test_client TestClient/TestClientMain.cpp)
target_link_libraries(remote_input_test_client
    PRIVATE remote_input_build_options remote_input_external_headers remote_input_warnings)

enable_testing()
add_test(NAME remote_input_tests COMMAND remote_input_tests)
