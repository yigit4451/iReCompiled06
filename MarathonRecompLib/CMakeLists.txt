project("MarathonRecompLib")

add_compile_options(
    -fno-strict-aliasing
)

if (WIN32)
    add_compile_options(/fp:strict)
else()
    add_compile_options(-ffp-model=strict)
endif()

set(GENERATED_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

set(XENON_RECOMP_IN_SWITCH_TABLE "${CMAKE_CURRENT_SOURCE_DIR}/config/switch_table.toml")
set(XENON_RECOMP_IN_XEX "${CMAKE_CURRENT_SOURCE_DIR}/private/default.xex")
set(XENON_RECOMP_OUT_DIR "${GENERATED_OUT_DIR}/ppc")
file(MAKE_DIRECTORY "${XENON_RECOMP_OUT_DIR}")

set(XENON_RECOMP_IN_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/config/Marathon.toml.in")
set(XENON_RECOMP_OUT_CONFIG_DIR "${CMAKE_CURRENT_BINARY_DIR}")
set(XENON_RECOMP_OUT_CONFIG "${XENON_RECOMP_OUT_CONFIG_DIR}/Marathon.toml")

# XenonRecomp wants paths relative to the config file directory.
file(RELATIVE_PATH XENON_RECOMP_IN_SWITCH_TABLE_REL "${XENON_RECOMP_OUT_CONFIG_DIR}" "${XENON_RECOMP_IN_SWITCH_TABLE}")
file(RELATIVE_PATH XENON_RECOMP_IN_XEX_REL "${XENON_RECOMP_OUT_CONFIG_DIR}" "${XENON_RECOMP_IN_XEX}")
file(RELATIVE_PATH XENON_RECOMP_OUT_DIR_REL "${XENON_RECOMP_OUT_CONFIG_DIR}" "${XENON_RECOMP_OUT_DIR}")

configure_file("${XENON_RECOMP_IN_CONFIG}" "${XENON_RECOMP_OUT_CONFIG}")

set(XENON_RECOMP_IN_PPC_CONTEXT "${CMAKE_CURRENT_SOURCE_DIR}/config/ppc_context.h")

set(XENOS_RECOMP_IN_DIR "${CMAKE_CURRENT_BINARY_DIR}/shader")
set(XENOS_RECOMP_OUT_DIR "${GENERATED_OUT_DIR}/shader")
set(XENOS_RECOMP_OUT_FILE "${XENOS_RECOMP_OUT_DIR}/shader_cache.cpp")
file(MAKE_DIRECTORY "${XENOS_RECOMP_IN_DIR}")
file(MAKE_DIRECTORY "${XENOS_RECOMP_OUT_DIR}")

# Generated shader_cache.cpp references header next to it.
set(XENOS_RECOMP_IN_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/shader/shader_cache.h")
set(XENOS_RECOMP_OUT_HEADER "${XENOS_RECOMP_OUT_DIR}/shader_cache.h")

add_custom_command(
    OUTPUT
        "${XENOS_RECOMP_OUT_HEADER}"
    COMMAND
        ${CMAKE_COMMAND} -E copy "${XENOS_RECOMP_IN_HEADER}" "${XENOS_RECOMP_OUT_HEADER}"
    DEPENDS
        "${XENOS_RECOMP_IN_HEADER}"
)

if (EXISTS "${XENON_RECOMP_IN_XEX}" AND
    EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/private/shader.arc" AND
    EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/private/shader_lt.arc")

    set(MARATHON_RECOMP_PPC_RECOMPILED_SOURCES
        "${XENON_RECOMP_OUT_DIR}/ppc_config.h"
        "${XENON_RECOMP_OUT_DIR}/ppc_context.h"
        "${XENON_RECOMP_OUT_DIR}/ppc_func_mapping.cpp"
        "${XENON_RECOMP_OUT_DIR}/ppc_recomp_shared.h"
    )

    foreach(i RANGE 0 145)
        list(APPEND MARATHON_RECOMP_PPC_RECOMPILED_SOURCES "${XENON_RECOMP_OUT_DIR}/ppc_recomp.${i}.cpp")
    endforeach()

    if (MARATHON_RECOMP_USE_PREBUILT_HOST_TOOLS)
        set(XENON_RECOMP_CMD "${MARATHON_RECOMP_HOST_XENON_RECOMP}")
        set(XENON_RECOMP_DEP "${MARATHON_RECOMP_HOST_XENON_RECOMP}")
        set(U8EXTRACT_CMD "${MARATHON_RECOMP_HOST_U8EXTRACT}")
        set(U8EXTRACT_DEP "${MARATHON_RECOMP_HOST_U8EXTRACT}")
        set(XENOS_RECOMP_CMD "${MARATHON_RECOMP_HOST_XENOS_RECOMP}")
        set(XENOS_RECOMP_DEP "${MARATHON_RECOMP_HOST_XENOS_RECOMP}")
    else()
        set(XENON_RECOMP_CMD "$<TARGET_FILE:XenonRecomp>")
        set(XENON_RECOMP_DEP XenonRecomp)
        set(U8EXTRACT_CMD "$<TARGET_FILE:u8extract>")
        set(U8EXTRACT_DEP u8extract)
        set(XENOS_RECOMP_CMD "$<TARGET_FILE:XenosRecomp>")
        set(XENOS_RECOMP_DEP XenosRecomp)
    endif()

    add_custom_command(
        OUTPUT
            ${MARATHON_RECOMP_PPC_RECOMPILED_SOURCES}
        COMMAND
            ${XENON_RECOMP_CMD} "${XENON_RECOMP_OUT_CONFIG}" "${XENON_RECOMP_IN_PPC_CONTEXT}"
        DEPENDS
            ${XENON_RECOMP_DEP}
            "${XENON_RECOMP_OUT_CONFIG}"
            "${XENON_RECOMP_IN_PPC_CONTEXT}"
            "${XENON_RECOMP_IN_SWITCH_TABLE}"
            "${XENON_RECOMP_IN_XEX}"
        USES_TERMINAL
    )

    add_custom_command(
        OUTPUT
            "${XENOS_RECOMP_IN_DIR}"
        COMMAND
            ${U8EXTRACT_CMD} "${CMAKE_CURRENT_SOURCE_DIR}/private/shader.arc" "${XENOS_RECOMP_IN_DIR}"
        COMMAND
            ${U8EXTRACT_CMD} "${CMAKE_CURRENT_SOURCE_DIR}/private/shader_lt.arc" "${XENOS_RECOMP_IN_DIR}"
        DEPENDS
            ${U8EXTRACT_DEP}
            "${CMAKE_CURRENT_SOURCE_DIR}/private/shader.arc"
            "${CMAKE_CURRENT_SOURCE_DIR}/private/shader_lt.arc"
    )

    set(XENOS_RECOMP_ROOT "${MARATHON_RECOMP_TOOLS_ROOT}/XenosRecomp/XenosRecomp")
    set(XENOS_RECOMP_INCLUDE "${XENOS_RECOMP_ROOT}/shader_common.h")

    if (TARGET XenosRecomp)
        target_compile_definitions(XenosRecomp PRIVATE MARATHON_RECOMP)
    endif()

    add_custom_command(
        OUTPUT
            "${XENOS_RECOMP_OUT_FILE}"
        COMMAND
            ${XENOS_RECOMP_CMD} "${XENOS_RECOMP_IN_DIR}" "${XENOS_RECOMP_OUT_FILE}" "${XENOS_RECOMP_INCLUDE}"
        DEPENDS
            ${XENOS_RECOMP_DEP}
            "${XENOS_RECOMP_IN_DIR}"
            ${XENOS_RECOMP_INCLUDE}
        USES_TERMINAL
    )
else()
    message(STATUS "MarathonRecompLib/private assets not found; generating stub PPC & shader cache units via tools/generate_stub_recomp.py")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    set(MARATHON_RECOMP_PPC_RECOMPILED_SOURCES
        "${XENON_RECOMP_OUT_DIR}/ppc_config.h"
        "${XENON_RECOMP_OUT_DIR}/ppc_context.h"
        "${XENON_RECOMP_OUT_DIR}/ppc_func_mapping.cpp"
        "${XENON_RECOMP_OUT_DIR}/ppc_recomp_shared.h"
        "${XENON_RECOMP_OUT_DIR}/ppc_recomp.0.cpp"
    )

    add_custom_command(
        OUTPUT
            ${MARATHON_RECOMP_PPC_RECOMPILED_SOURCES}
            "${XENOS_RECOMP_OUT_FILE}"
        COMMAND
            Python3::Interpreter "${MARATHON_RECOMP_TOOLS_ROOT}/generate_stub_recomp.py"
                --scan-dir "${CMAKE_SOURCE_DIR}/MarathonRecomp"
                --ppc-context-in "${XENON_RECOMP_IN_PPC_CONTEXT}"
                --ppc-out-dir "${XENON_RECOMP_OUT_DIR}"
                --shader-out-file "${XENOS_RECOMP_OUT_FILE}"
        DEPENDS
            "${MARATHON_RECOMP_TOOLS_ROOT}/generate_stub_recomp.py"
            "${XENON_RECOMP_IN_PPC_CONTEXT}"
        USES_TERMINAL
    )
endif()

add_library(MarathonRecompLib
    ${MARATHON_RECOMP_PPC_RECOMPILED_SOURCES}
    "${XENOS_RECOMP_OUT_HEADER}"
    "${XENOS_RECOMP_OUT_FILE}"
)

target_include_directories(MarathonRecompLib PUBLIC ${GENERATED_OUT_DIR})
target_include_directories(MarathonRecompLib PRIVATE "${MARATHON_RECOMP_TOOLS_ROOT}/XenonRecomp/thirdparty/simde")
target_precompile_headers(MarathonRecompLib PUBLIC "${XENON_RECOMP_OUT_DIR}/ppc_recomp_shared.h")
