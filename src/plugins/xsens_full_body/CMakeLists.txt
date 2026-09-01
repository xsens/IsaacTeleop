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
    frame_decision.cpp
)

target_link_libraries(xsens_full_body_plugin PRIVATE
    pusherio::pusherio
    oxr::oxr_core
    isaacteleop_schema
)

install(TARGETS xsens_full_body_plugin RUNTIME DESTINATION plugins/xsens_full_body)
install(FILES plugin.yaml README.md DESTINATION plugins/xsens_full_body)

# Unit test for the frame decision pipeline. It links frame_decision.cpp directly and needs no
# OpenXR runtime, so it runs anywhere -- the same shape as tests/cpp/plugins/oglo_tactile.
#
# It lives here rather than under tests/cpp/plugins/ on purpose: that tree is gated on
# BUILD_PLUGIN_OGLO and registering there would mean patching two more upstream CMake files.
# Everything in this directory is ours, so keeping the target here holds the Xsens layer's
# upstream footprint at the 7 registration points documented in isaacteleop_xsens/README.md.
if(BUILD_TESTING)
    add_executable(test_xsens_frame_decision
        tests/test_frame_decision.cpp
        frame_decision.cpp
    )
    target_include_directories(test_xsens_frame_decision PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_xsens_frame_decision PRIVATE isaacteleop_schema)
    add_test(NAME xsens_frame_decision COMMAND test_xsens_frame_decision)
endif()
