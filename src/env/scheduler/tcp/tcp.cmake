# Find liburing
find_library(URING_LIBRARY NAMES uring liburing)
if(NOT URING_LIBRARY)
    message(FATAL_ERROR "liburing not found. Please install liburing-dev.")
endif()

# Include directories for liburing
find_path(URING_INCLUDE_DIR NAMES liburing.h)
if(NOT URING_INCLUDE_DIR)
    message(FATAL_ERROR "liburing headers not found. Please install liburing-dev.")
endif()
include_directories(${URING_INCLUDE_DIR})