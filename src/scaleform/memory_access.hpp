#pragma once

#include <Windows.h>

namespace sf {

// Exception filter for __try blocks that read game memory. Only access
// violations and in-page errors are handled; other exceptions keep propagating.
[[nodiscard]] inline int MemoryAccessExceptionFilter(
    unsigned long exceptionCode) noexcept {
    return exceptionCode == EXCEPTION_ACCESS_VIOLATION ||
            exceptionCode == EXCEPTION_IN_PAGE_ERROR
        ? EXCEPTION_EXECUTE_HANDLER
        : EXCEPTION_CONTINUE_SEARCH;
}

}
