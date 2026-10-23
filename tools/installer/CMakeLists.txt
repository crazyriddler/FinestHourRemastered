# Finest Hour Remastered's installer: a Dear ImGui window on Direct3D 11 (imgui_SOURCE_DIR from the SDK's FetchContent)
# the update inside it unpacked by the LZMA SDK's decoder (public domain, from 7-Zip's sources): no part of Windows, so
# Wine runs it too
include(FetchContent)
FetchContent_Declare(lzma_sdk GIT_REPOSITORY https://github.com/ip7z/7zip.git GIT_TAG 24.09 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(lzma_sdk)
add_executable(FinestHourRemasteredSetup WIN32
    main.cpp xdvdfs.cpp installer.rc ${lzma_sdk_SOURCE_DIR}/C/LzmaDec.c
    ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)
# the resources' files: a changed picture or font makes the resources again
file(GLOB INSTALLER_ART CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/art/*)
set_source_files_properties(installer.rc PROPERTIES OBJECT_DEPENDS "${INSTALLER_ART}")
target_include_directories(FinestHourRemasteredSetup PRIVATE ${imgui_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR} ${lzma_sdk_SOURCE_DIR}/C)
target_compile_definitions(FinestHourRemasteredSetup PRIVATE UNICODE _UNICODE)
target_link_libraries(FinestHourRemasteredSetup PRIVATE d3d11 dxgi bcrypt ole32 shell32 windowscodecs dwmapi)
if(MSVC)
    target_compile_options(FinestHourRemasteredSetup PRIVATE /utf-8)
    set_property(TARGET FinestHourRemasteredSetup PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()
