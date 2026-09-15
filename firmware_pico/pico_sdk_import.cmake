# This is a copy of <PICO_SDK_PATH>/external/pico_sdk_import.cmake

if (DEFINED ENV{PICO_SDK_PATH} AND (NOT PICO_SDK_PATH))
    set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
    message("Using PICO_SDK_PATH from environment ('${PICO_SDK_PATH}')")
endif ()

if (DEFINED PICO_SDK_PATH)
    # Validate SDK path
    if (NOT EXISTS ${PICO_SDK_PATH})
        message(FATAL_ERROR "Directory specified by PICO_SDK_PATH '${PICO_SDK_PATH}' does not exist")
    endif ()
    set(PICO_SDK_PATH "${PICO_SDK_PATH}" CACHE PATH "Path to the Raspberry Pi Pico SDK")
else ()
    # Check if SDK is available in sibling or parent directories
    get_filename_component(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/../pico-sdk" REALPATH)
    if (NOT EXISTS ${PICO_SDK_PATH})
        get_filename_component(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/pico-sdk" REALPATH)
    endif ()
    if (NOT EXISTS ${PICO_SDK_PATH})
        message(FATAL_ERROR "PICO_SDK_PATH not set and SDK not found. Please set PICO_SDK_PATH environment variable or pass -DPICO_SDK_PATH=...")
    endif ()
endif ()

set(PICO_SDK_PATH "${PICO_SDK_PATH}" CACHE PATH "Path to the Raspberry Pi Pico SDK")
include(${PICO_SDK_PATH}/pico_sdk_init.cmake)
