#pragma once

// Used only by the CMake build with REMOTE_INPUT_NO_EXCEPTIONS=ON, which checks that RemoteInput builds and
// runs with C++ exceptions disabled. ASIO then reports failures that it would otherwise throw by calling
// asio::detail::throw_exception(), which the program has to define. RemoteInput uses the overloads that report
// failure through an asio::error_code wherever ASIO offers one, so this is only reached if the OS refuses ASIO a
// resource it cannot do without, such as its event queue or its name lookup thread. A game built this way would
// define it to hand over to its crash handler, e.g. with INT3_ASSERT.

#if defined(__GNUC__)
#pragma GCC system_header   // ASIO's signature names a parameter this definition does not use
#endif

#include <cstdio>
#include <cstdlib>

#include "asio/detail/throw_exception.hpp"

namespace asio::detail
{
template <typename Exception>
void throw_exception(const Exception& exception ASIO_SOURCE_LOCATION_PARAM)
{
    std::fprintf(stderr, "ASIO tried to throw with exceptions disabled: %s\n", exception.what());
    std::abort();
}
} // namespace asio::detail
