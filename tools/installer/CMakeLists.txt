# Finest Hour Remastered's installer: a Dear ImGui window on Direct3D 11 (imgui_SOURCE_DIR from the SDK's FetchContent)
add_executable(FinestHourRemasteredSetup WIN32
    main.cpp xdvdfs.cpp installer.rc
    ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
# the resources' files: a changed picture or font makes the resources again
file(GLOB INSTALLER_ART CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/art/*)
set_source_files_properties(installer.rc PROPERTIES OBJECT_DEPENDS "${INSTALLER_ART}")
target_include_directories(FinestHourRemasteredSetup PRIVATE ${imgui_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(FinestHourRemasteredSetup PRIVATE UNICODE _UNICODE)
target_link_libraries(FinestHourRemasteredSetup PRIVATE d3d11 dxgi bcrypt ole32 shell32 windowscodecs dwmapi)
if(MSVC)
    target_compile_options(FinestHourRemasteredSetup PRIVATE /utf-8)
    set_property(TARGET FinestHourRemasteredSetup PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()
