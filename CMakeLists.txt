cmake_minimum_required(VERSION 3.15)
project(DMX LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)  # needed for shared libs

set(SOURCE_FILES DMX.cpp)

# Include directories for headers
include_directories(
    ${CMAKE_SOURCE_DIR}/serial/include
    ${CMAKE_SOURCE_DIR}/libartnet/include
    ${CMAKE_SOURCE_DIR}/sACN/include
    ${CMAKE_SOURCE_DIR}/chuck/include
)

# Add sACN as a subdirectory (which includes EtcPal)
add_subdirectory(sACN)

# Set your external static library paths for serial and libartnet (adjust paths as needed)
if(WIN32)
    set(SERIAL_LIBRARY "${CMAKE_SOURCE_DIR}/serial/build/Release/serial.lib")
    set(ARTNET_LIBRARY "${CMAKE_SOURCE_DIR}/libartnet/msvc/libartnet/x64/Release/libartnet.lib")
else()
    set(SERIAL_LIBRARY "${CMAKE_SOURCE_DIR}/serial/lib/libserial.a")
    set(ARTNET_LIBRARY "${CMAKE_SOURCE_DIR}/libartnet/lib/libartnet.a")
endif()

# Platform system libraries
if(WIN32)
    set(PLATFORM_LIBS ws2_32 mswsock ole32 user32 advapi32 kernel32 rpcrt4 winmm iphlpapi)
else()
    set(PLATFORM_LIBS pthread stdc++)
endif()

# Define your chugin library target
add_library(${PROJECT_NAME} SHARED ${SOURCE_FILES})
set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX ".chug")

# Link libraries: static serial + libartnet + sACN (which includes etcpal) + platform libs
target_link_libraries(${PROJECT_NAME}
    ${SERIAL_LIBRARY}
    ${ARTNET_LIBRARY}
    sACN           # sACN added as subproject target
    ${PLATFORM_LIBS}
)
