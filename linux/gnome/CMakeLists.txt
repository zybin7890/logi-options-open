# GNOME 46 exposes Meta-14. Build only when the optional GNOME provider is selected.
enable_language(C)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MUTTER REQUIRED IMPORTED_TARGET libmutter-14)
find_program(G_IR_SCANNER g-ir-scanner REQUIRED)
find_program(G_IR_COMPILER g-ir-compiler REQUIRED)
add_library(logi-clipboard SHARED clipboard-source.c)
set_target_properties(logi-clipboard PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED YES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/native")
target_link_libraries(logi-clipboard PRIVATE PkgConfig::MUTTER)
target_include_directories(logi-clipboard PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(logi-clipboard PRIVATE -Wall -Wextra -Wpedantic)
set(gir "${CMAKE_CURRENT_BINARY_DIR}/native/LogiClipboard-1.0.gir")
set(typelib "${CMAKE_CURRENT_BINARY_DIR}/native/LogiClipboard-1.0.typelib")
pkg_get_variable(mutter_girdir libmutter-14 girdir)
if(NOT mutter_girdir)
    pkg_get_variable(mutter_libdir libmutter-14 libdir)
    set(mutter_girdir "${mutter_libdir}/mutter-14")
endif()
add_custom_command(OUTPUT "${gir}"
    COMMAND ${CMAKE_COMMAND} -E env "CC=${CMAKE_C_COMPILER}"
        ${G_IR_SCANNER} --warn-all --namespace=LogiClipboard --nsversion=1.0
        --identifier-prefix=LogiClipboard --symbol-prefix=logi_clipboard
        --include=GObject-2.0 --include=Gio-2.0 --include=Meta-14
        "--add-include-path=${mutter_girdir}" --pkg=libmutter-14
        "--library-path=$<TARGET_FILE_DIR:logi-clipboard>" --library=logi-clipboard
        "--output=${gir}" "${CMAKE_CURRENT_SOURCE_DIR}/clipboard-source.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/clipboard-source.c"
    DEPENDS logi-clipboard clipboard-source.h clipboard-source.c VERBATIM)
add_custom_command(OUTPUT "${typelib}"
    COMMAND ${G_IR_COMPILER} "--includedir=${mutter_girdir}" "${gir}" "--output=${typelib}"
    DEPENDS "${gir}" VERBATIM)
add_custom_target(logi-clipboard-introspection ALL DEPENDS "${typelib}")
install(TARGETS logi-clipboard LIBRARY DESTINATION share/logi-options-open/gnome/native)
install(FILES "${typelib}" "${gir}" DESTINATION share/logi-options-open/gnome/native)
