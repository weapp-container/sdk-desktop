cmake_minimum_required(VERSION 3.21)
project(WeappDesktopNativeSources LANGUAGES CXX)
include(CTest)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(WEAPP_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
find_package(Threads REQUIRED)
add_executable(sensor_lifecycle ../device_sensor.cpp ../../src/value.cpp)
target_include_directories(sensor_lifecycle PRIVATE "${WEAPP_SOURCE_ROOT}/src" "${WEAPP_SOURCE_ROOT}/include" "${WEAPP_SOURCE_ROOT}/third_party")
target_link_libraries(sensor_lifecycle PRIVATE Threads::Threads)
add_test(NAME sensor_lifecycle COMMAND sensor_lifecycle)
if(WIN32)
  # This target uses the actual Windows SDK to compile every native wrapper
  # translation unit. An OBJECT target intentionally performs no core link:
  # private Windows runtime archives are not available in this source checkout.
  add_library(windows_native_sources OBJECT ../../src/windows.cpp ../../src/device_apis_windows.cpp ../../src/desktop.cpp ../../src/value.cpp ../../src/c_api.cpp ../../src/native_preview.cpp winrt_sensors.cpp)
  target_include_directories(windows_native_sources PRIVATE "${WEAPP_SOURCE_ROOT}/src" "${WEAPP_SOURCE_ROOT}/include" "${WEAPP_SOURCE_ROOT}/third_party")
  target_compile_definitions(windows_native_sources PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00 WEAPP_DEVICE_WINRT_LOCATION=1 WEAPP_DEVICE_WINRT_BLUETOOTH=1 WEAPP_DEVICE_WINRT_SENSORS=1)
  target_compile_definitions(sensor_lifecycle PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
  # DeviceCommon's Win32 file helpers remain compiled in the finite test.
  target_link_libraries(sensor_lifecycle PRIVATE bcrypt ole32)
endif()
