cmake_minimum_required(VERSION 4.1)
project(LogiNative LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "This candidate targets Windows only")
endif()
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
configure_file(../default-windows.ini mappings.ini COPYONLY)
configure_file(../bridge.py bridge.py COPYONLY)

add_library(logi-core STATIC hidpp.cpp mouse.cpp remap.cpp ring.cpp)
target_link_libraries(logi-core PUBLIC setupapi hid shell32 advapi32)

add_executable(logi-cli main.cpp)
target_link_libraries(logi-cli PRIVATE logi-core)
foreach(asset side.png front.png back.png loginative.ico firefox.png chrome.png wps.png)
    configure_file("../assets/${asset}" "assets/${asset}" COPYONLY)
endforeach()
add_executable(logi-native WIN32 native.cpp smart_native.cpp assets.rc)
target_link_libraries(logi-native PRIVATE logi-core comctl32 comdlg32 gdiplus ole32 dwmapi shlwapi uxtheme)

foreach(target logi-core logi-cli logi-native)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO)
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
                               WINVER=0x0A00 _WIN32_WINNT=0x0A00)
    if(MINGW)
        target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic>")
        target_link_options(${target} PRIVATE -municode -static $<$<CONFIG:Release>:-s>)
    endif()
endforeach()

include(CTest)
if(BUILD_TESTING)
    if(NOT MINGW)
        message(FATAL_ERROR "The isolated Windows tests currently require MinGW-w64; use BUILD_TESTING=OFF for other compilers")
    endif()
    add_subdirectory(tests)
endif()
