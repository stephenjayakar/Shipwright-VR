cmake_minimum_required(VERSION 3.26.0 FATAL_ERROR)

set(CMAKE_SYSTEM_VERSION 10.0 CACHE STRING "" FORCE)

project(soh LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20 CACHE STRING "The C++ standard to use")
set(CMAKE_C_STANDARD 23 CACHE STRING "The C standard to use")

if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    enable_language(OBJCXX)
    set(CMAKE_OBJC_FLAGS "${CMAKE_OBJC_FLAGS} -fobjc-arc")
    set(CMAKE_OBJCXX_FLAGS "${CMAKE_OBJCXX_FLAGS} -fobjc-arc")
endif()

set (BUILD_UTILS OFF CACHE STRING "no utilities")
set (BUILD_SHARED_LIBS OFF CACHE STRING "install/link shared instead of static libs")

################################################################################
# RTX DX12 Raytracing Option
################################################################################
if (CMAKE_SYSTEM_NAME STREQUAL "Windows" AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    option(ENABLE_DX12_RTX "Enable DirectX 12 RTX raytracing for Kokiri Forest" ON)
else()
    set(ENABLE_DX12_RTX OFF CACHE BOOL "Enable DirectX 12 RTX raytracing (Windows x64 only)" FORCE)
endif()

################################################################################
# Set target arch type if empty. Visual studio solution generator provides it.
################################################################################
if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
	if(NOT CMAKE_VS_PLATFORM_NAME)
		set(CMAKE_VS_PLATFORM_NAME "x64")
	endif()
	message("${CMAKE_VS_PLATFORM_NAME} architecture in use")

	if(NOT ("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "x64"
		OR "${CMAKE_VS_PLATFORM_NAME}" STREQUAL "Win32"))
		message(FATAL_ERROR "${CMAKE_VS_PLATFORM_NAME} arch is not supported!")
	endif()
endif()

################################################################################
# Global configuration types
################################################################################
set(CMAKE_CONFIGURATION_TYPES
    "Debug"
    "Release"
    CACHE STRING "" FORCE
)

################################################################################
# Global compiler options
################################################################################
if(MSVC)
    # remove default flags provided with CMake for MSVC
    set(CMAKE_C_FLAGS "")
    set(CMAKE_C_FLAGS_DEBUG "")
    set(CMAKE_C_FLAGS_RELEASE "")
    set(CMAKE_CXX_FLAGS "")
    set(CMAKE_CXX_FLAGS_DEBUG "")
    set(CMAKE_CXX_FLAGS_RELEASE "")
endif()

################################################################################
# Global linker options
################################################################################
if(MSVC)
    # remove default flags provided with CMake for MSVC
    set(CMAKE_EXE_LINKER_FLAGS "")
    set(CMAKE_MODULE_LINKER_FLAGS "")
    set(CMAKE_SHARED_LINKER_FLAGS "")
    set(CMAKE_STATIC_LINKER_FLAGS "")
    set(CMAKE_EXE_LINKER_FLAGS_DEBUG "${CMAKE_EXE_LINKER_FLAGS}")
    set(CMAKE_MODULE_LINKER_FLAGS_DEBUG "${CMAKE_MODULE_LINKER_FLAGS}")
    set(CMAKE_SHARED_LINKER_FLAGS_DEBUG "${CMAKE_SHARED_LINKER_FLAGS}")
    set(CMAKE_STATIC_LINKER_FLAGS_DEBUG "${CMAKE_STATIC_LINKER_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS}")
    set(CMAKE_MODULE_LINKER_FLAGS_RELEASE "${CMAKE_MODULE_LINKER_FLAGS}")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS}")
    set(CMAKE_STATIC_LINKER_FLAGS_RELEASE "${CMAKE_STATIC_LINKER_FLAGS}")
endif()

################################################################################
# Common utils
################################################################################
include(CMake/Utils.cmake)

################################################################################
# Additional Global Settings(add specific info there)
################################################################################
include(CMake/GlobalSettingsInclude.cmake OPTIONAL)

################################################################################
# Use solution folders feature
################################################################################
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

################################################################################
# Sub-projects
################################################################################
if (NOT TARGET libultraship)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../libultraship ${CMAKE_BINARY_DIR}/libultraship)
endif()

if (NOT TARGET ZAPDLib)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../ZAPDTR/ZAPD ${CMAKE_BINARY_DIR}/ZAPD)
endif()

set(PROJECT_NAME soh)

################################################################################
# Sources
################################################################################
configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/src/boot/build.c.in ${CMAKE_CURRENT_SOURCE_DIR}/src/boot/build.c @ONLY)
configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/properties.h.in ${CMAKE_CURRENT_SOURCE_DIR}/properties.h @ONLY)

set(Header_Files "resource.h")
source_group("headers" FILES ${Header_Files})

# include {{{
file(GLOB Header_Files__include "include/*.h" "include/*.inc")
list(APPEND Header_Files__include ${CMAKE_CURRENT_SOURCE_DIR}/include/libc/stdarg.h)
list(REMOVE_ITEM Header_Files__include ${CMAKE_CURRENT_SOURCE_DIR}/include/bgm.h)
list(REMOVE_ITEM Header_Files__include ${CMAKE_CURRENT_SOURCE_DIR}/include/math_n64.h)
list(REMOVE_ITEM Header_Files__include ${CMAKE_CURRENT_SOURCE_DIR}/include/stdbool_n64.h)
list(REMOVE_ITEM Header_Files__include ${CMAKE_CURRENT_SOURCE_DIR}/include/stddef_n64.h)
list(REMOVE_ITEM Header_Files__include ${CMAKE_CURRENT_SOURCE_DIR}/include/stdlib_n64.h)
list(REMOVE_ITEM Header_Files__include ${CMAKE_CURRENT_SOURCE_DIR}/include/ultra64.h)
source_group("include" FILES ${Header_Files__include})
# }}}

# soh (root)
file(GLOB_RECURSE soh__ RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "soh/*.c" "soh/*.cpp" "soh/*.h" "soh/*.hpp")

# Add specific files that don't match the pattern
list(APPEND soh__ ${CMAKE_CURRENT_SOURCE_DIR}/soh/Enhancements/savestates_extern.inc)
list(APPEND soh__ ${CMAKE_CURRENT_SOURCE_DIR}/soh/Enhancements/speechsynthesizer/DarwinSpeechSynthesizer.mm)

# Create source groups that match the real file directory paths
foreach(source IN LISTS soh__)
    get_filename_component(source_path "${source}" PATH)
    string(REPLACE "/" "\\" source_path_adjusted "${source_path}")
    source_group("${source_path_adjusted}" FILES "${source}")
endforeach()

# Fix UTF-8 issues for MSVC
if (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set_source_files_properties(soh/OTRGlobals.cpp PROPERTIES COMPILE_FLAGS "/utf-8")
    set_source_files_properties(soh/Enhancements/tts/tts.cpp PROPERTIES COMPILE_FLAGS "/utf-8")
    set_source_files_properties(soh/Enhancements/custom-message/CustomMessageManager.cpp PROPERTIES COMPILE_FLAGS "/utf-8")
    set_source_files_properties(soh/Enhancements/custom-message/CustomMessageManager.h PROPERTIES COMPILE_FLAGS "/utf-8")
endif()

# handle Network removals
if (!BUILD_REMOTE_CONTROL)
    list(FILTER soh__ EXCLUDE REGEX "soh/Enhancements/crowd-control/")
endif()

# handle speechsynthesizer removals
if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    list(FILTER soh__ EXCLUDE REGEX "soh/Enhancements/speechsynthesizer/Darwin")
elseif (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    list(FILTER soh__ EXCLUDE REGEX "soh/Enhancements/speechsynthesizer/SAPI")
else()
    list(FILTER soh__ EXCLUDE REGEX "soh/Enhancements/speechsynthesizer/(Darwin|SAPI)")
endif()

# handle RTX removals
if (NOT ENABLE_DX12_RTX)
    list(FILTER soh__ EXCLUDE REGEX "soh/Enhancements/RTX/")
endif()

find_library(ESPEAK espeak-ng)
if (NOT ESPEAK)
    list(FILTER soh__ EXCLUDE REGEX "soh/Enhancements/speechsynthesizer/ESpeak")
endif()

# soh/Extractor {{{
list(FILTER ship__ EXCLUDE REGEX "soh/Extractor/*")

if(NOT CMAKE_SYSTEM_NAME MATCHES "NintendoSwitch|CafeOS")
    file(GLOB_RECURSE soh__Extractor RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
        "soh/Extractor/*.c"
        "soh/Extractor/*.cpp"
        "soh/Extractor/*.h"
        "soh/Extractor/*.hpp"
    )
else()
    file(GLOB_RECURSE soh__Extractor RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
        "soh/Extractor/*.h"
        "soh/Extractor/*.hpp"
    )
endif()
# }}}

# src (decomp) {{{
file(GLOB_RECURSE src__ RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "src/*.c" "src/*.h")
set_source_files_properties(${src__} PROPERTIES COMPILE_OPTIONS "${WARNING_OVERRIDE}")

list(APPEND src__ ${CMAKE_CURRENT_SOURCE_DIR}/Resource.rc)
list(FILTER src__ EXCLUDE REGEX "src/dmadata/")
list(FILTER src__ EXCLUDE REGEX "src/elf_message/")
list(FILTER src__ EXCLUDE REGEX "src/libultra/io/")
list(FILTER src__ EXCLUDE REGEX "src/libultra/libc/")
list(FILTER src__ EXCLUDE REGEX "src/libultra/os/")
list(FILTER src__ EXCLUDE REGEX "src/libultra/rmon/")
list(APPEND src__ "src/libultra/libc/sprintf.c")
list(REMOVE_ITEM src__ "src/libultra/gu/cosf.c")
list(REMOVE_ITEM src__ "src/libultra/gu/lookat.c")
list(REMOVE_ITEM src__ "src/libultra/gu/lookathil.c")
list(REMOVE_ITEM src__ "src/libultra/gu/perspective.c")
list(REMOVE_ITEM src__ "src/libultra/gu/position.c")
list(REMOVE_ITEM src__ "src/libultra/gu/sinf.c")
list(REMOVE_ITEM src__ "src/libultra/gu/sinf.c")
list(REMOVE_ITEM src__ "src/libultra/gu/sqrtf.c")
list(REMOVE_ITEM src__ "src/libultra/gu/us2dex.c")

source_group("src" REGULAR_EXPRESSION "src/*")
source_group("src\\boot" REGULAR_EXPRESSION "src/boot/*")
source_group("src\\buffers" REGULAR_EXPRESSION "src/buffers/*")
source_group("src\\code" REGULAR_EXPRESSION "src/code/*")
source_group("src\\libultra" REGULAR_EXPRESSION "src/libultra/*")
source_group("src\\overlays\\actors" REGULAR_EXPRESSION "src/overlays/actors/*")
source_group("src\\overlays\\effects" REGULAR_EXPRESSION "src/overlays/effects/*")
source_group("src\\overlays\\gamestates" REGULAR_EXPRESSION  "src/overlays/gamestates/*")
source_group("src\\overlays\\misc" REGULAR_EXPRESSION  "src/overlays/misc/*")
# }}}

set(ALL_FILES
    ${Header_Files}
    ${Header_Files__include}
    ${soh__}
    ${soh__Extractor}
    ${src__}
)

################################################################################
# Target
################################################################################
add_executable(${PROJECT_NAME} ${ALL_FILES})

################################################################################
# RTX Build System Integration
################################################################################
if (ENABLE_DX12_RTX AND CMAKE_SYSTEM_NAME STREQUAL "Windows")

    set(RTX_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/soh/Enhancements/RTX)
    set(RTX_SHADER_DIR ${RTX_SOURCE_DIR}/Shaders)

    set(RTX_SOURCES
        ${RTX_SOURCE_DIR}/RTXTypes.h
        ${RTX_SOURCE_DIR}/DX12Context.h
        ${RTX_SOURCE_DIR}/DX12Device.h
        ${RTX_SOURCE_DIR}/DX12Renderer.h
        ${RTX_SOURCE_DIR}/RTXPipeline.h
        ${RTX_SOURCE_DIR}/DXRPipeline.h
        ${RTX_SOURCE_DIR}/RTAccelStructure.h
        ${RTX_SOURCE_DIR}/AccelerationStructure.h
        ${RTX_SOURCE_DIR}/RTXManager.h
        ${RTX_SOURCE_DIR}/RTXRenderer.h
        ${RTX_SOURCE_DIR}/RTXSceneManager.h
        ${RTX_SOURCE_DIR}/SceneAdapter.h
        ${RTX_SOURCE_DIR}/SceneGeometryExtractor.h
        ${RTX_SOURCE_DIR}/TextureManager.h
        ${RTX_SOURCE_DIR}/GISystem.h
        ${RTX_SOURCE_DIR}/RTXSceneConfig.h
        ${RTX_SOURCE_DIR}/RTXHooks.h
        ${RTX_SOURCE_DIR}/RTXShaderCompiler.h
        ${RTX_SOURCE_DIR}/RTXDiagLog.h
        ${RTX_SOURCE_DIR}/RTXDiagnostics.h
        ${RTX_SOURCE_DIR}/RTXScreenCapture.h
        ${RTX_SOURCE_DIR}/UICompositor.h
        ${RTX_SOURCE_DIR}/DX12Context.cpp
        ${RTX_SOURCE_DIR}/DX12Device.cpp
        ${RTX_SOURCE_DIR}/DX12Renderer.cpp
        ${RTX_SOURCE_DIR}/RTXPipeline.cpp
        ${RTX_SOURCE_DIR}/DXRPipeline.cpp
        ${RTX_SOURCE_DIR}/RTAccelStructure.cpp
        ${RTX_SOURCE_DIR}/AccelerationStructure.cpp
        ${RTX_SOURCE_DIR}/RTXManager.cpp
        ${RTX_SOURCE_DIR}/RTXRenderer.cpp
        ${RTX_SOURCE_DIR}/RTXSceneManager.cpp
        ${RTX_SOURCE_DIR}/SceneAdapter.cpp
        ${RTX_SOURCE_DIR}/SceneConverter.cpp
        ${RTX_SOURCE_DIR}/SceneGeometryExtractor.cpp
        ${RTX_SOURCE_DIR}/TextureManager.cpp
        ${RTX_SOURCE_DIR}/GISystem.cpp
        ${RTX_SOURCE_DIR}/RTXSceneConfig.cpp
        ${RTX_SOURCE_DIR}/RTXHooks.cpp
        ${RTX_SOURCE_DIR}/RTXShaderCompiler.cpp
        ${RTX_SOURCE_DIR}/RTXDiagnostics.cpp
        ${RTX_SOURCE_DIR}/RTXScreenCapture.cpp
        ${RTX_SOURCE_DIR}/UICompositor.cpp
        ${RTX_SOURCE_DIR}/DX12Bridge.cpp
    )

    set(RTX_SHADER_SOURCES
        ${RTX_SHADER_DIR}/Common.hlsli
        ${RTX_SHADER_DIR}/RayGen.hlsl
        ${RTX_SHADER_DIR}/ClosestHit.hlsl
        ${RTX_SHADER_DIR}/Miss.hlsl
        ${RTX_SHADER_DIR}/AnyHit.hlsl
        ${RTX_SHADER_DIR}/Denoise.hlsl
        ${RTX_SHADER_DIR}/RTXGlobalIllumination.hlsl
        ${RTX_SHADER_DIR}/RTXRaytracing.hlsl
        ${RTX_SHADER_DIR}/PostProcess.hlsl
    )
    if (EXISTS ${RTX_SHADER_DIR}/Accumulate.hlsl)
        list(APPEND RTX_SHADER_SOURCES ${RTX_SHADER_DIR}/Accumulate.hlsl)
    endif()
    if (EXISTS ${RTX_SHADER_DIR}/Composite.hlsl)
        list(APPEND RTX_SHADER_SOURCES ${RTX_SHADER_DIR}/Composite.hlsl)
    endif()

    set_source_files_properties(${RTX_SHADER_SOURCES} PROPERTIES HEADER_FILE_ONLY TRUE)

    target_sources(${PROJECT_NAME} PRIVATE
        ${RTX_SOURCES}
        ${RTX_SHADER_SOURCES}
    )

    target_include_directories(${PROJECT_NAME} PRIVATE
        ${RTX_SOURCE_DIR}
    )

    source_group("soh\\Enhancements\\RTX" FILES ${RTX_SOURCES})
    source_group("soh\\Enhancements\\RTX\\Shaders" FILES ${RTX_SHADER_SOURCES})

    # =========================================================================
    # Shader Compilation (DXC)
    # =========================================================================
    set(RTX_SHADER_OUTPUT_DIR ${CMAKE_BINARY_DIR}/shaders)
    file(MAKE_DIRECTORY ${RTX_SHADER_OUTPUT_DIR})

    set(DXC_SEARCH_PATHS
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64/dxc.exe"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/dxc.exe"
    )
    set(DXC_EXE "")
    foreach(_path ${DXC_SEARCH_PATHS})
        if(EXISTS "${_path}")
            set(DXC_EXE "${_path}")
            break()
        endif()
    endforeach()

    if(DXC_EXE STREQUAL "")
        find_program(DXC_EXE NAMES dxc HINTS
            "$ENV{WindowsSdkDir}bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
        )
    endif()

    if(DXC_EXE)
        message(STATUS "DXC found at: ${DXC_EXE}")

        set(RTX_RT_SHADERS RayGen ClosestHit Miss AnyHit RTXRaytracing)
        # CS shaders: NAME;ENTRY_POINT pairs
        set(RTX_CS_SHADER_Denoise_ENTRY "Denoise")
        set(RTX_CS_SHADER_RTXGlobalIllumination_ENTRY "GlobalIllumination")
        set(RTX_CS_SHADER_Accumulate_ENTRY "Accumulate")
        set(RTX_CS_SHADER_PostProcess_ENTRY "PostProcess")
        set(RTX_CS_SHADER_Composite_ENTRY "Composite")
        set(RTX_CS_SHADERS Denoise RTXGlobalIllumination Accumulate PostProcess Composite)

        set(RTX_CSO_OUTPUTS "")

        foreach(SHADER ${RTX_RT_SHADERS})
            if(EXISTS "${RTX_SHADER_DIR}/${SHADER}.hlsl")
                set(CSO_OUT "${RTX_SHADER_OUTPUT_DIR}/${SHADER}.cso")
                add_custom_command(
                    OUTPUT ${CSO_OUT}
                    COMMAND "${DXC_EXE}" -T lib_6_3 -I "${RTX_SHADER_DIR}" -Fo "${CSO_OUT}" "${RTX_SHADER_DIR}/${SHADER}.hlsl"
                    DEPENDS "${RTX_SHADER_DIR}/${SHADER}.hlsl" "${RTX_SHADER_DIR}/Common.hlsli"
                    COMMENT "Compiling RT shader ${SHADER}.hlsl -> ${SHADER}.cso"
                    VERBATIM
                )
                list(APPEND RTX_CSO_OUTPUTS ${CSO_OUT})
            endif()
        endforeach()

        foreach(SHADER ${RTX_CS_SHADERS})
            if(EXISTS "${RTX_SHADER_DIR}/${SHADER}.hlsl")
                set(CSO_OUT "${RTX_SHADER_OUTPUT_DIR}/${SHADER}.cso")
                set(_entry "${RTX_CS_SHADER_${SHADER}_ENTRY}")
                add_custom_command(
                    OUTPUT ${CSO_OUT}
                    COMMAND "${DXC_EXE}" -T cs_6_0 -E ${_entry} -I "${RTX_SHADER_DIR}" -Fo "${CSO_OUT}" "${RTX_SHADER_DIR}/${SHADER}.hlsl"
                    DEPENDS "${RTX_SHADER_DIR}/${SHADER}.hlsl" "${RTX_SHADER_DIR}/Common.hlsli"
                    COMMENT "Compiling CS shader ${SHADER}.hlsl (entry: ${_entry}) -> ${SHADER}.cso"
                    VERBATIM
                )
                list(APPEND RTX_CSO_OUTPUTS ${CSO_OUT})
            endif()
        endforeach()

        add_custom_target(RTXShaders ALL DEPENDS ${RTX_CSO_OUTPUTS})
        add_dependencies(${PROJECT_NAME} RTXShaders)

        # Deploy CSOs and HLSL sources to the exe output directory.
        # We use POST_BUILD on RTXShaders (not on soh) so deployment happens
        # even when soh.exe doesn't need relinking. This is critical because
        # shader-only changes don't trigger a soh relink, but the shaders
        # still need to be deployed to the output directory.
        #
        # Deploy to ALL potential output directories to cover any CMake config.
        # ${CMAKE_BINARY_DIR} is the build directory (e.g., build/)
        # The actual exe may end up in various locations depending on generator.
        set(RTX_DEPLOY_DIRS
            "${CMAKE_SOURCE_DIR}/x64/Release/shaders"
            "${CMAKE_SOURCE_DIR}/x64/Debug/shaders"
            "${CMAKE_BINARY_DIR}/Release/shaders"
            "${CMAKE_BINARY_DIR}/Debug/shaders"
            "${CMAKE_BINARY_DIR}/shaders"
        )
        foreach(_deploy_dir ${RTX_DEPLOY_DIRS})
            # Deploy CSO files
            foreach(CSO_FILE ${RTX_CSO_OUTPUTS})
                get_filename_component(CSO_NAME ${CSO_FILE} NAME)
                add_custom_command(TARGET RTXShaders POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory "${_deploy_dir}"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${CSO_FILE}" "${_deploy_dir}/${CSO_NAME}"
                    COMMENT "Deploying ${CSO_NAME} to ${_deploy_dir}"
                )
            endforeach()
        endforeach()

        # Also deploy HLSL source files alongside CSOs.
        # This ensures runtime DXC compilation has current sources available.
        # The DXRPipeline code deletes CSOs and compiles from HLSL at runtime,
        # so having up-to-date HLSL sources in the output dir is essential.
        # Both .hlsl and .hlsli files are deployed (Common.hlsli is required for #includes).
        #
        # IMPORTANT: We use copy_directory on RTXShaders POST_BUILD instead of
        # individual file copies on soh POST_BUILD. This ensures deployment
        # happens whenever shaders are recompiled, not just when soh is relinked.
        foreach(_deploy_dir ${RTX_DEPLOY_DIRS})
            add_custom_command(TARGET RTXShaders POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${_deploy_dir}"
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${RTX_SHADER_DIR}" "${_deploy_dir}"
                COMMENT "Deploying HLSL source files to ${_deploy_dir}"
            )
        endforeach()

        # Also deploy via soh POST_BUILD as a safety net (covers first build).
        # $<TARGET_FILE_DIR:soh> resolves to the ACTUAL exe output directory,
        # which is the most reliable path. This ensures HLSL sources and CSOs
        # are always next to the exe regardless of CMake generator or config.
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/shaders"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${RTX_SHADER_DIR}" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/shaders"
            COMMENT "Deploying shader sources to output directory (via TARGET_FILE_DIR)"
        )
        foreach(CSO_FILE ${RTX_CSO_OUTPUTS})
            get_filename_component(CSO_NAME ${CSO_FILE} NAME)
            add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CSO_FILE}" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/shaders/${CSO_NAME}"
                COMMENT "Copying ${CSO_NAME} to output (via TARGET_FILE_DIR)"
            )
        endforeach()

        # Log deployment paths at build time for debugging
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E echo "RTX Shader deployment: source=${RTX_SHADER_DIR}, target=$<TARGET_FILE_DIR:${PROJECT_NAME}>/shaders"
            COMMENT "RTX shader deployment path info"
        )
    else()
        message(WARNING "DXC not found. RTX shaders will not be compiled.")
    endif()

    # RTX compile definitions
    # Use file(TO_CMAKE_PATH) to ensure forward slashes in the path define,
    # preventing Windows backslash escaping issues in the C++ string literal.
    file(TO_CMAKE_PATH "${RTX_SHADER_DIR}" RTX_SHADER_DIR_FWD)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        ENABLE_DX12_RTX=1
        RTX_SHADER_SOURCE_DIR="${RTX_SHADER_DIR_FWD}"
        RTX_COMPILED_SHADER_SUBDIR="shaders"
    )

    # Post-build: Copy DXC runtime DLLs
    set(DXC_DLL_SEARCH_PATHS
        "${CMAKE_SOURCE_DIR}/dxc/bin/x64"
        "$ENV{WindowsSdkDir}bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64"
    )
    set(DXC_DLLS dxcompiler.dll dxil.dll)
    foreach(DXC_DLL ${DXC_DLLS})
        set(_found FALSE)
        foreach(_dir ${DXC_DLL_SEARCH_PATHS})
            if(EXISTS "${_dir}/${DXC_DLL}")
                add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${_dir}/${DXC_DLL}"
                        "$<TARGET_FILE_DIR:${PROJECT_NAME}>/${DXC_DLL}"
                    COMMENT "Copying ${DXC_DLL} from ${_dir}"
                )
                set(_found TRUE)
                break()
            endif()
        endforeach()
        if(NOT _found)
            message(WARNING "DXC runtime DLL '${DXC_DLL}' not found. "
                "Place it in ${CMAKE_SOURCE_DIR}/dxc/bin/x64/.")
        endif()
    endforeach()

    message(STATUS "RTX DX12 raytracing support: ENABLED")
else()
    if(NOT ENABLE_DX12_RTX)
        message(STATUS "RTX DX12 raytracing support: DISABLED")
    else()
        message(STATUS "RTX DX12 raytracing support: DISABLED (Windows x64 only)")
    endif()
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
use_props(${PROJECT_NAME} "${CMAKE_CONFIGURATION_TYPES}" "${DEFAULT_CXX_PROPS}")
endif()

set(ROOT_NAMESPACE soh)

if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
	set_target_properties(${PROJECT_NAME} PROPERTIES
		VS_GLOBAL_KEYWORD "Win32Proj"
	)
	if("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "x64")
		set_target_properties(${PROJECT_NAME} PROPERTIES
			INTERPROCEDURAL_OPTIMIZATION_RELEASE "TRUE"
		)
	elseif("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "Win32")
		set_target_properties(${PROJECT_NAME} PROPERTIES
			INTERPROCEDURAL_OPTIMIZATION_RELEASE "TRUE"
		)
	endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set_target_properties(${PROJECT_NAME} PROPERTIES
        XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES
        OUTPUT_NAME "soh-macos"
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	set_target_properties(${PROJECT_NAME} PROPERTIES
	OUTPUT_NAME "soh.elf"
	)
endif()
################################################################################
# MSVC runtime library
################################################################################
if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
	get_property(MSVC_RUNTIME_LIBRARY_DEFAULT TARGET ${PROJECT_NAME} PROPERTY MSVC_RUNTIME_LIBRARY)
	if("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "x64")
		string(CONCAT "MSVC_RUNTIME_LIBRARY_STR"
			$<$<CONFIG:Debug>:
				MultiThreadedDebug
			>
			$<$<CONFIG:Release>:
				MultiThreaded
			>
			$<$<NOT:$<OR:$<CONFIG:Debug>,$<CONFIG:Release>>>:${MSVC_RUNTIME_LIBRARY_DEFAULT}>
		)
	elseif("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "Win32")
		string(CONCAT "MSVC_RUNTIME_LIBRARY_STR"
			$<$<CONFIG:Debug>:
				MultiThreadedDebug
			>
			$<$<CONFIG:Release>:
				MultiThreaded
			>
			$<$<NOT:$<OR:$<CONFIG:Debug>,$<CONFIG:Release>>>:${MSVC_RUNTIME_LIBRARY_DEFAULT}>
		)
	endif()
	set_target_properties(${PROJECT_NAME} PROPERTIES MSVC_RUNTIME_LIBRARY ${MSVC_RUNTIME_LIBRARY_STR})
endif()
################################################################################
# Find/download Dr Libs (For custom audio)
################################################################################
include(FetchContent)

FetchContent_Declare(
        dr_libs
        GIT_REPOSITORY https://github.com/mackron/dr_libs.git
        GIT_TAG da35f9d6c7374a95353fd1df1d394d44ab66cf01
)
FetchContent_MakeAvailable(dr_libs)

find_package(SDL2)
set(SDL2-INCLUDE ${SDL2_INCLUDE_DIRS})

if (BUILD_REMOTE_CONTROL)
    find_package(SDL2_net)

    if(NOT SDL2_net_FOUND)
        message(STATUS "SDL2_net not found (it's possible the version installed is too old). Disabling BUILD_REMOTE_CONTROL.")
        set(BUILD_REMOTE_CONTROL 0)
    else()
        set(SDL2-NET-INCLUDE ${SDL_NET_INCLUDE_DIRS})
    endif()
endif()

if (ESPEAK)
    add_compile_definitions(ESPEAK=1)
endif()

################################################################################
# Compile definitions
################################################################################

target_include_directories(${PROJECT_NAME} PRIVATE assets
	${CMAKE_CURRENT_SOURCE_DIR}/include/
	${CMAKE_CURRENT_SOURCE_DIR}/src/
    ${CMAKE_CURRENT_SOURCE_DIR}/../libultraship/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../ZAPDTR/ZAPD/resource/type
	${SDL2-INCLUDE}
    ${SDL2-NET-INCLUDE}
	${CMAKE_CURRENT_SOURCE_DIR}/assets/
	${dr_libs_SOURCE_DIR}
	.
)

if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
	if("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "x64")
		target_compile_definitions(${PROJECT_NAME} PRIVATE
			"$<$<CONFIG:Debug>:"
				"_DEBUG;"
				"_CRT_SECURE_NO_WARNINGS;"
				"ENABLE_DX11;"
			">"
			"$<$<CONFIG:Release>:"
				"NDEBUG;"
			">"
            "$<$<BOOL:${BUILD_REMOTE_CONTROL}>:ENABLE_REMOTE_CONTROL>"
			"INCLUDE_GAME_PRINTF;"
            "F3DEX_GBI_2"
			"UNICODE;"
			"_UNICODE"
            SPDLOG_ACTIVE_LEVEL=${SPDLOG_MIN_CUTOFF}
            LOG_LEVEL_GAME_PRINTS=${SPDLOG_LEVEL_OFF}
			STORMLIB_NO_AUTO_LINK
			"_CRT_SECURE_NO_WARNINGS;"
            NOMINMAX
			"$<$<BOOL:${ENABLE_DX12_RTX}>:ENABLE_DX12_RTX=1>"
		)
	elseif("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "Win32")
		target_compile_definitions(${PROJECT_NAME} PRIVATE
			"$<$<CONFIG:Debug>:"
				"NOINCLUDE_GAME_PRINTF;"
				"_DEBUG;"
				"_CRT_SECURE_NO_WARNINGS;"
				"ENABLE_OPENGL;"
			">"
			"$<$<CONFIG:Release>:"
				"NDEBUG;"
			">"
			"INCLUDE_GAME_PRINTF;"
            "F3DEX_GBI_2"
			"WIN32;"
			"UNICODE;"
			"_UNICODE;"
            SPDLOG_ACTIVE_LEVEL=${SPDLOG_MIN_CUTOFF}
            LOG_LEVEL_GAME_PRINTS=${SPDLOG_LEVEL_OFF}
			STORMLIB_NO_AUTO_LINK
            NOMINMAX
		)
	endif()
elseif (CMAKE_SYSTEM_NAME STREQUAL "CafeOS")
	target_compile_definitions(${PROJECT_NAME} PRIVATE
		"$<$<CONFIG:Debug>:"
			"_DEBUG;"
		">"
		"$<$<CONFIG:Release>:"
			"NDEBUG;"
		">"
        "F3DEX_GBI_2;"
		"SPDLOG_NO_THREAD_ID;"
        "SPDLOG_NO_TLS;"
		"STBI_NO_THREAD_LOCALS;"
        SPDLOG_ACTIVE_LEVEL=${SPDLOG_MIN_CUTOFF}
        LOG_LEVEL_GAME_PRINTS=${SPDLOG_LEVEL_OFF}
	)
elseif ("${CMAKE_CXX_COMPILER_ID}" MATCHES "GNU|Clang|AppleClang")
	target_compile_definitions(${PROJECT_NAME} PRIVATE
		"$<$<CONFIG:Debug>:"
			"_DEBUG;"
		">"
		"$<$<CONFIG:Release>:"
			"NDEBUG;"
		">"
        "F3DEX_GBI_2;"
        "$<$<BOOL:${BUILD_REMOTE_CONTROL}>:ENABLE_REMOTE_CONTROL>;"
		"_CONSOLE;"
		"_CRT_SECURE_NO_WARNINGS;"
		"ENABLE_OPENGL;"
		"UNICODE;"
		"_UNICODE;"
        SPDLOG_ACTIVE_LEVEL=${SPDLOG_MIN_CUTOFF}
        LOG_LEVEL_GAME_PRINTS=${SPDLOG_LEVEL_OFF}
	)
endif()
################################################################################
# Compile and link options
################################################################################
if(MSVC)
    if("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "x64")
        target_compile_options(${PROJECT_NAME} PRIVATE
            $<$<CONFIG:Debug>:
                /w;
                /Od
            >
            $<$<CONFIG:Release>:
                /Oi;
                /Gy;
                /W3
            >
            /bigobj;
            /sdl-;
            /permissive-;
            /MP;
            ${DEFAULT_CXX_DEBUG_INFORMATION_FORMAT};
            ${DEFAULT_CXX_EXCEPTION_HANDLING}
        )
        target_compile_options(${PROJECT_NAME} PRIVATE  $<$<CONFIG:Debug>:/ZI;>)
    elseif("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "Win32")
        target_compile_options(${PROJECT_NAME} PRIVATE
            $<$<CONFIG:Debug>:
                /RTCs
            >
            $<$<CONFIG:Release>:
                /O2;
                /Oi;
                /Gy
            >
            /permissive-;
            /MP;
            /sdl-;
            /w;
            ${DEFAULT_CXX_DEBUG_INFORMATION_FORMAT};
            ${DEFAULT_CXX_EXCEPTION_HANDLING}
        )
    endif()
    if("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "x64")
        target_link_options(${PROJECT_NAME} PRIVATE
            $<$<CONFIG:Debug>:
                /INCREMENTAL
            >
            $<$<CONFIG:Release>:
                /OPT:REF;
                /OPT:ICF;
                /INCREMENTAL:NO;
                /FORCE:MULTIPLE
            >
            /MANIFEST:NO;
            /DEBUG;
            /SUBSYSTEM:WINDOWS
        )
    elseif("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "Win32")
        target_link_options(${PROJECT_NAME} PRIVATE
            $<$<CONFIG:Debug>:
                /STACK:8777216
            >
            $<$<CONFIG:Release>:
                /OPT:REF;
                /OPT:ICF;
                /INCREMENTAL:NO;
                /FORCE:MULTIPLE
            >
            /MANIFEST:NO;
            /DEBUG;
            /SUBSYSTEM:WINDOWS
        )
    endif()
endif()

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        target_compile_options(${PROJECT_NAME} PRIVATE
            -Wall -Wextra -Wno-error
            -Wformat-security
            -Wno-return-type
            -Wno-unused-parameter
            -Wno-unused-function
            -Wno-unused-variable
            -Wno-missing-field-initializers
            -Wno-parentheses
            -Wno-narrowing
            -Wno-missing-braces
            $<$<COMPILE_LANGUAGE:C>:
                -Werror-implicit-function-declaration
                -Wno-incompatible-pointer-types
                -Wno-int-conversion
            >
            $<$<COMPILE_LANGUAGE:CXX>:-fpermissive>
            $<$<COMPILE_LANGUAGE:CXX>:
                -Wno-c++11-narrowing
                -Wno-deprecated-enum-enum-conversion
            >
            -pthread
        )

        target_link_options(${PROJECT_NAME} PRIVATE
            -pthread
        )
    elseif (CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
        target_compile_options(${PROJECT_NAME} PRIVATE
            -Wall -Wextra -Wno-error
            -Wformat-security
            -Wno-return-type
            -Wno-unused-parameter
            -Wno-unused-function
            -Wno-unused-variable
            -Wno-missing-field-initializers
            -Wno-parentheses
            -Wno-narrowing
            -Wno-missing-braces
            $<$<COMPILE_LANGUAGE:C>:
                -Werror-implicit-function-declaration
                -Wno-incompatible-pointer-types
            >
            $<$<COMPILE_LANGUAGE:CXX>:-fpermissive>
            $<$<COMPILE_LANGUAGE:CXX>:
                -Wno-c++11-narrowing
                -Wno-deprecated-enum-enum-conversion
            >
            -pthread
        )

        target_link_options(${PROJECT_NAME} PRIVATE
            -pthread
        )
    elseif (CMAKE_SYSTEM_NAME STREQUAL "CafeOS")
        target_compile_options(${PROJECT_NAME} PRIVATE
            -O2

            # disable some warnings to not clutter output
            -Wno-multichar
            -Wno-return-type
            -Wno-narrowing
            -Wno-switch-outside-range
            $<$<COMPILE_LANGUAGE:C>:
                -Werror-implicit-function-declaration
                -Wno-incompatible-pointer-types
                -Wno-discarded-array-qualifiers
                -Wno-discarded-qualifiers
                -Wno-int-conversion
                -Wno-builtin-declaration-mismatch
                -Wno-switch-unreachable
                -Wno-stringop-overflow
            >
        )
    else()
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
		set(CPU_OPTION -msse2 -mfpmath=sse)
        endif()

        target_compile_options(${PROJECT_NAME} PRIVATE
            -Wall -Wextra -Wno-error
            -Wformat-security
            -Wno-unused-parameter
            -Wno-unused-function
            -Wno-unused-variable
            -Wno-missing-field-initializers
            -Wno-parentheses
            -Wno-narrowing
            -Wno-missing-braces
            $<$<COMPILE_LANGUAGE:C>:
                -Werror-implicit-function-declaration
                -Wno-implicit-int
                -Wno-incompatible-pointer-types
                -Wno-int-conversion
            >
            $<$<COMPILE_LANGUAGE:CXX>:-fpermissive>
            $<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated-enum-enum-conversion>
            -pthread
	    ${CPU_OPTION}
        )

        target_link_options(${PROJECT_NAME} PRIVATE
            -pthread
				#-fsanitize=address
            -Wl,-export-dynamic
        )
    endif()
endif()
################################################################################
# Pre build events
################################################################################
if (CMAKE_GENERATOR MATCHES "Visual Studio")
	add_custom_command(
    TARGET ${PROJECT_NAME}
	POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different ${CMAKE_SOURCE_DIR}/soh/assets/extractor ${CMAKE_BINARY_DIR}/soh/assets
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different ${CMAKE_SOURCE_DIR}/soh/assets/xml ${CMAKE_BINARY_DIR}/soh/assets/xml
    )
endif()
if(NOT CMAKE_SYSTEM_NAME MATCHES "NintendoSwitch|CafeOS")
    add_custom_command(
		TARGET ${PROJECT_NAME}
		POST_BUILD
		COMMENT "Copying asset xmls..."
        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different ${CMAKE_SOURCE_DIR}/soh/assets/extractor $<TARGET_FILE_DIR:soh>/assets
		COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different ${CMAKE_SOURCE_DIR}/soh/assets/xml $<TARGET_FILE_DIR:soh>/assets/xml
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:soh>/assets/symbols
	#	COMMAND ${VS_COPY_ASSETS_CMD}
    )
endif()
################################################################################
# Dependencies
################################################################################
add_dependencies(${PROJECT_NAME}
    libultraship
)
if(NOT CMAKE_SYSTEM_NAME MATCHES "NintendoSwitch|CafeOS")
add_dependencies(${PROJECT_NAME}
    ZAPDLib
)
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
    find_package(glfw3 REQUIRED)
	find_package(Ogg CONFIG REQUIRED)
	link_libraries(Ogg::ogg)

	find_package(Vorbis CONFIG REQUIRED)
	link_libraries(Vorbis::vorbisfile)
	find_package(Opus CONFIG REQUIRED)
	link_libraries(Opus::opus)
	find_package(OpusFile CONFIG REQUIRED)
	link_libraries(OpusFile::opusfile CONFIG REQUIRED)
	if("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "x64")
		set(ADDITIONAL_LIBRARY_DEPENDENCIES
			"libultraship;"
			"ZAPDLib;"
			"glu32;"
			"SDL2::SDL2;"
			"SDL2::SDL2main;"
            "$<$<BOOL:${BUILD_REMOTE_CONTROL}>:SDL2_net::SDL2_net-static>"
			"glfw;"
			"winmm;"
			"imm32;"
			"version;"
			"setupapi"
			"Ogg::ogg"
			"Opus::opus"
			"Vorbis::vorbis"
			"Vorbis::vorbisenc"
			"Vorbis::vorbisfile"
			"OpusFile::opusfile"
			"$<$<BOOL:${ENABLE_DX12_RTX}>:d3d12>"
			"$<$<BOOL:${ENABLE_DX12_RTX}>:dxgi>"
			"$<$<BOOL:${ENABLE_DX12_RTX}>:d3dcompiler>"
			"$<$<BOOL:${ENABLE_DX12_RTX}>:dxguid>"
			"$<$<BOOL:${ENABLE_DX12_RTX}>:dxcompiler>"
		)
	elseif("${CMAKE_VS_PLATFORM_NAME}" STREQUAL "Win32")
		set(ADDITIONAL_LIBRARY_DEPENDENCIES
			"libultraship;"
			"ZAPDLib;"
			"glu32;"
			"SDL2::SDL2;"
			"SDL2::SDL2main;"
			"glfw;"
			"winmm;"
			"imm32;"
			"version;"
			"setupapi"
		)
	endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
    find_package(SDL2)
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
    set(ADDITIONAL_LIBRARY_DEPENDENCIES
        "libultraship;"
        SDL2::SDL2
        -lglad
        Threads::Threads
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "CafeOS")
    find_package(SDL2 REQUIRED)
    set(ADDITIONAL_LIBRARY_DEPENDENCIES
        "libultraship;"
        SDL2::SDL2-static

        "$<$<CONFIG:Debug>:-Wl,--wrap=abort>"
    )
    target_include_directories(${PROJECT_NAME} PRIVATE
        ${DEVKITPRO}/portlibs/wiiu/include/
    )
else()
    find_package(SDL2)
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
	find_package(Threads REQUIRED)
	find_package(Ogg REQUIRED)
	find_package(Vorbis REQUIRED)
	find_package(Opus REQUIRED)
	find_package(OpusFile REQUIRED)
 	set(ADDITIONAL_LIBRARY_DEPENDENCIES
        "libultraship;"
		"ZAPDLib;"
		SDL2::SDL2
		"Ogg::ogg"
		"Vorbis::vorbis"
		"Vorbis::vorbisenc"
		"Vorbis::vorbisfile"
		"Opus::opus"
		"Opusfile::Opusfile"
        "$<$<BOOL:${BUILD_REMOTE_CONTROL}>:SDL2_net::SDL2_net>"
		${CMAKE_DL_LIBS}
		Threads::Threads
	)
endif()

if(NOT CMAKE_SYSTEM_NAME MATCHES "Darwin|NintendoSwitch|CafeOS")
INSTALL(TARGETS soh DESTINATION . COMPONENT ship)
endif()

if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
INSTALL(FILES $<TARGET_PDB_FILE:soh> DESTINATION ./debug COMPONENT ship)
INSTALL(FILES ${CMAKE_BINARY_DIR}/soh/soh.o2r DESTINATION . COMPONENT ship)
endif()

find_program(CURL NAMES curl DOC "Path to the curl program.  Used to download files.")
if (NOT EXISTS ${CMAKE_BINARY_DIR}/gamecontrollerdb.txt)
    execute_process(COMMAND ${CURL} -sSfL https://raw.githubusercontent.com/mdqinc/SDL_GameControllerDB/master/gamecontrollerdb.txt -o ${CMAKE_BINARY_DIR}/gamecontrollerdb.txt OUTPUT_VARIABLE RESULT)
endif()

if("${CMAKE_SYSTEM_NAME}" STREQUAL "Darwin")
configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/macosx/Info.plist.in ${CMAKE_BINARY_DIR}/macosx/Info.plist @ONLY)
INSTALL(TARGETS soh DESTINATION ../MacOS COMPONENT ship)
INSTALL(FILES ${CMAKE_BINARY_DIR}/gamecontrollerdb.txt DESTINATION ../MacOS COMPONENT ship)
INSTALL(FILES ${CMAKE_BINARY_DIR}/soh/soh.o2r DESTINATION ../Resources COMPONENT ship)
elseif(NOT "${CMAKE_SYSTEM_NAME}" MATCHES "NintendoSwitch|CafeOS")
INSTALL(FILES ${CMAKE_BINARY_DIR}/gamecontrollerdb.txt DESTINATION . COMPONENT ship)
endif()

if(CMAKE_SYSTEM_NAME MATCHES "NintendoSwitch|CafeOS")
    if (NOT TARGET pathconf)
        add_library(pathconf OBJECT platform/pathconf.c)
    endif()
    target_link_libraries(${PROJECT_NAME} PRIVATE "${ADDITIONAL_LIBRARY_DEPENDENCIES}" $<TARGET_OBJECTS:pathconf> )
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE "${ADDITIONAL_LIBRARY_DEPENDENCIES}")
endif()

if(CMAKE_SYSTEM_NAME MATCHES "NintendoSwitch")

nx_generate_nacp(Ship.nacp
   NAME "Ship of Harkinian"
   AUTHOR "${PROJECT_TEAM}"
   VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}"
)

nx_create_nro(soh
    NACP Ship.nacp
    ICON ${CMAKE_CURRENT_SOURCE_DIR}/icon.jpg
)

INSTALL(FILES ${CMAKE_CURRENT_BINARY_DIR}/soh.nro DESTINATION . COMPONENT ship)

elseif(CMAKE_SYSTEM_NAME MATCHES "CafeOS")

wut_create_rpx(${PROJECT_NAME})

wut_create_wuhb(${PROJECT_NAME}
	NAME       "Ship of Harkinian"
	SHORTNAME  "SoH"
	AUTHOR     "${PROJECT_TEAM}"
	ICON       ${CMAKE_CURRENT_SOURCE_DIR}/icon.jpg
)

INSTALL(FILES ${CMAKE_CURRENT_BINARY_DIR}/soh.rpx ${CMAKE_CURRENT_BINARY_DIR}/soh.wuhb DESTINATION . COMPONENT ship)

endif()
