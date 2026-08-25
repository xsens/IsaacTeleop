# SPDX-FileCopyrightText: Copyright (c) 2026 Xsens Technologies B.V. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS "Skipping xsens_full_body plugin (Linux only)")
    add_custom_target(xsens_full_body_plugin
        COMMAND ${CMAKE_COMMAND} -E echo "Skipping xsens_full_body: Linux only")
    return()
endif()

# No vendor SDK: MVN Studio performs the skeleton conversion itself and emits the schema this
# repo already defines, so the plugin needs nothing beyond the core libraries.
add_executable(xsens_full_body_plugin
    main.cpp
    xsens_full_body_plugin.cpp
)

target_link_libraries(xsens_full_body_plugin PRIVATE
    pusherio::pusherio
    oxr::oxr_core
    isaacteleop_schema
)

install(TARGETS xsens_full_body_plugin RUNTIME DESTINATION plugins/xsens_full_body)
install(FILES plugin.yaml README.md DESTINATION plugins/xsens_full_body)
