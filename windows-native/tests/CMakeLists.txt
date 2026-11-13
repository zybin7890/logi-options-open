add_executable(logi-gesture-tests events.cpp)
set_target_properties(logi-gesture-tests PROPERTIES CXX_STANDARD 26 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
target_compile_definitions(logi-gesture-tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00)
target_compile_options(logi-gesture-tests PRIVATE -ffunction-sections -fdata-sections)
target_link_options(logi-gesture-tests PRIVATE -static -Wl,--gc-sections)
target_link_libraries(logi-gesture-tests PRIVATE user32 shell32 advapi32)
set(cases
    phase-contract tap direction diagonal multi-direction-final
    app-switch switch-away-back same-window-new-pid null-capture null-to-valid-held
    foreground-lost macro-app-switch mouse-open failed-open-handoff smart-open
    concurrent-runs-physical-ctrl ring-unreturned ring-restored loss-cancel stop-cancel
    reconnect-cancel reload-cancel dpi-cancel dpi-window-released dpi-window-held
    pan-tap pan-progress pan-cancel physical-modifier partial-keyboard
    partial-keyboard-two partial-keyboard-three rejected-input partial-mouse
    failed-cleanup-new-physical-hold)
foreach(case IN LISTS cases)
    add_test(NAME gesture-${case} COMMAND logi-gesture-tests "${case}" "${CMAKE_CURRENT_BINARY_DIR}/${case}.ini")
    set_tests_properties(gesture-${case} PROPERTIES TIMEOUT 10)
endforeach()

add_executable(logi-media-tests media_events.cpp)
set_target_properties(logi-media-tests PROPERTIES CXX_STANDARD 26 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
target_compile_definitions(logi-media-tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00)
target_compile_options(logi-media-tests PRIVATE -ffunction-sections -fdata-sections)
target_link_options(logi-media-tests PRIVATE -static -Wl,--gc-sections)
target_link_libraries(logi-media-tests PRIVATE user32 shell32 advapi32)
set(media_cases parser tap first-packet below-start start-equal action-equal action-above
    left up down negative-equal tie accumulate reverse large-packet restart duration-boundary
    duration-after duration-vertical vertical-repeat idle focus-before focus-after loss stop
    reconnect reload dpi partial rejected physical-key)
foreach(case IN LISTS media_cases)
    add_test(NAME media-${case} COMMAND logi-media-tests "${case}" "${CMAKE_CURRENT_BINARY_DIR}/media-${case}.ini")
    set_tests_properties(media-${case} PROPERTIES TIMEOUT 10)
endforeach()
