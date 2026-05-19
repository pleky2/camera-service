cmake_minimum_required(VERSION 3.10)
project(CameraService)

set(CMAKE_CXX_STANDARD 17)
add_definitions(-DUNICODE -D_UNICODE)

# Add include directories
include_directories(include sony_ptp)

# Add all source files
file(GLOB SOURCES "src/*.cpp" "sony_ptp/*.cpp")

# Create executable
add_executable(CameraService ${SOURCES})

# Link Windows libraries needed by httplib and PTP/WIA
target_link_libraries(CameraService ws2_32 crypt32 ole32 oleaut32 gdi32 user32 windowscodecs PortableDeviceGuids wiaguid)
