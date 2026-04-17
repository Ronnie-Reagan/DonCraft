#pragma once

#include "core/log.hpp"

#include <cstdlib>

#if defined(_MSC_VER)
#define DF_DEBUG_BREAK() __debugbreak()
#else
#define DF_DEBUG_BREAK() ((void)0)
#endif

#define DF_ASSERT(condition, message)                                                                            \
    do                                                                                                           \
    {                                                                                                            \
        if (!(condition))                                                                                        \
        {                                                                                                        \
            ::df::LogError("Assertion failed: ", #condition, " | ", message, " @ ", __FILE__, ":", __LINE__);  \
            DF_DEBUG_BREAK();                                                                                    \
            std::abort();                                                                                        \
        }                                                                                                        \
    } while (false)

#define DF_UNREACHABLE(message) DF_ASSERT(false, message)
