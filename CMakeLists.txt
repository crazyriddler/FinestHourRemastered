# Call of Duty: Finest Hour (original Xbox), statically recompiled

cmake_minimum_required(VERSION 3.25)
project(codfh LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# the SDK: sdk/ here, two folders up inside the SDK's templates, a checkout beside this repository, else downloaded
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/sdk/cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/sdk)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../..)
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../XboxXBE-Recomp/cmake/XbeRecomp.cmake)
    set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../XboxXBE-Recomp)
else()
    include(FetchContent)
    FetchContent_Declare(xbe_sdk GIT_REPOSITORY https://github.com/TGP482/XboxXBE-Recomp.git GIT_TAG main)
    FetchContent_MakeAvailable(xbe_sdk)
    set(SDK ${xbe_sdk_SOURCE_DIR})
endif()
# reproducible: the same sources and toolchain make the same executables, wherever they are built (no folder in them,
# no link time: /Brepro stamps a hash of the contents, the PDB named alone). The release is built with LLVM 21.1.0,
# MSVC 14.44.35207 and the Windows SDK 10.0.26100.0, the C runtime linked in (no Visual C++ redistributable needed)
# (no RTTI, unused: its type names carry anonymous namespaces' names, which clang derives from each file's absolute
# path; no time in the objects)
add_compile_options("-ffile-prefix-map=${CMAKE_SOURCE_DIR}/=" "-ffile-prefix-map=${CMAKE_BINARY_DIR}/="
                    "-mno-incremental-linker-compatible" "$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>")
add_link_options("LINKER:/Brepro" "LINKER:/pdbaltpath:%_PDB%")
option(XBE_SYMBOLS "line tables and a PDB for crash dumps and profiles (the exe then differs from the release's)" OFF)
set(XBE_RELEASE_TOOLCHAIN "21.1.0")
if(NOT CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL XBE_RELEASE_TOOLCHAIN)
    message(WARNING "Clang ${CMAKE_CXX_COMPILER_VERSION}: the release is built with ${XBE_RELEASE_TOOLCHAIN}; this build "
                    "works but is not byte for byte the release's")
endif()

include(${SDK}/cmake/XbeRecomp.cmake)


# the installer of the release (tools/installer)
add_subdirectory(tools/installer)

file(GLOB XBE_HOST_SOURCES CONFIGURE_DEPENDS src/*/*.cpp)
list(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/game/")

# partitions as xbe_symbols wrote them into config/game.toml
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS config/game.toml)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/config/game.toml)
    file(STRINGS config/game.toml GAME_PARTITIONS REGEX "^partitions *=")
    string(REGEX MATCH "[0-9]+" GAME_PARTITIONS "${GAME_PARTITIONS}")
endif()
# no config yet: build.bat writes it with xbe_symbols, the change reconfigures
if(NOT GAME_PARTITIONS)
    set(GAME_PARTITIONS 8)
endif()

# target name = GAME_EXE in build.bat, xbe = GAME_XBE
add_xbe_image(CODFH default.xbe game.toml ${GAME_PARTITIONS} game)
target_sources(CODFH PRIVATE src/codfh.rc)
set_source_files_properties(src/codfh.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/installer/art/app.ico)
