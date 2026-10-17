cmake_minimum_required(VERSION 3.19)
project(hymission VERSION "0.11.0")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(CTest)
include(GNUInstallDirs)

add_compile_definitions(WLR_USE_UNSTABLE)
add_compile_options(
  -g
  -Wall
  -Wextra
  -Wno-unused-parameter
  -Wno-unused-value
  -Wno-missing-field-initializers
  -Wno-narrowing
  -Wno-pointer-arith
  --no-gnu-unique
)

if(CMAKE_EXPORT_COMPILE_COMMANDS)
  set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES
      ${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES})
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(HYPR_DEPS REQUIRED hyprland)
pkg_check_modules(LUA REQUIRED lua)
pkg_check_modules(GLIB REQUIRED glib-2.0)
pkg_check_modules(GTK4 REQUIRED gtk4)
pkg_check_modules(ADWAITA REQUIRED IMPORTED_TARGET libadwaita-1)
pkg_check_modules(GTK4_LAYER_SHELL REQUIRED gtk4-layer-shell-0)
# nlohmann/json is vendored as a single header in src/vendor; no system package required.

add_library(hymission SHARED
  src/main.cpp
  src/mission_layout.cpp
  src/overview_logic.cpp
  src/overview_controller.cpp
  src/stage_logic.cpp
  src/stage_controller.cpp
  src/stage_edge_effect.cpp
)

target_include_directories(hymission PRIVATE ${HYPR_DEPS_INCLUDE_DIRS} ${LUA_INCLUDE_DIRS} ${GLIB_INCLUDE_DIRS})

foreach(inc ${HYPR_DEPS_INCLUDE_DIRS})
  if(EXISTS "${inc}/hyprland/src")
    target_include_directories(hymission PRIVATE "${inc}/hyprland/src")
  endif()
endforeach()

target_link_directories(hymission PRIVATE ${HYPR_DEPS_LIBRARY_DIRS} ${LUA_LIBRARY_DIRS} ${GLIB_LIBRARY_DIRS})
target_link_libraries(hymission PRIVATE ${HYPR_DEPS_LIBRARIES} ${LUA_LIBRARIES} ${GLIB_LIBRARIES})

add_executable(hymission-search-input src/search_input.cpp)
target_include_directories(hymission-search-input PRIVATE ${GTK4_INCLUDE_DIRS} ${GTK4_LAYER_SHELL_INCLUDE_DIRS})
target_link_directories(hymission-search-input PRIVATE ${GTK4_LIBRARY_DIRS} ${GTK4_LAYER_SHELL_LIBRARY_DIRS})
target_link_libraries(hymission-search-input PRIVATE ${GTK4_LIBRARIES} ${GTK4_LAYER_SHELL_LIBRARIES})
target_link_libraries(hymission-search-input PRIVATE PkgConfig::ADWAITA)

add_executable(hymission-layout-demo
  tools/layout_demo.cpp
  src/mission_layout.cpp
)

target_include_directories(hymission-layout-demo PRIVATE src)

if(BUILD_TESTING)
  add_executable(hymission-config-command-test tools/config_command_test.cpp)
  target_include_directories(hymission-config-command-test PRIVATE src)
  add_test(NAME hymission-config-command-test COMMAND hymission-config-command-test)
  add_executable(hymission-state-json-test tools/state_json_test.cpp)
  target_include_directories(hymission-state-json-test PRIVATE src)
  add_test(NAME hymission-state-json-test COMMAND hymission-state-json-test)
  add_executable(hymission-search-focus-test tools/search_focus_test.cpp)
  target_include_directories(hymission-search-focus-test PRIVATE src)
  add_test(NAME hymission-search-focus-test COMMAND hymission-search-focus-test)
  set_tests_properties(hymission-search-focus-test PROPERTIES TIMEOUT 5)
  add_executable(hymission-deferred-snapshot-test tools/deferred_snapshot_test.cpp)
  target_include_directories(hymission-deferred-snapshot-test PRIVATE src)
  add_test(NAME hymission-deferred-snapshot-test COMMAND hymission-deferred-snapshot-test)
  add_executable(hymission-input-guard-test tools/input_guard_test.cpp)
  target_include_directories(hymission-input-guard-test PRIVATE src)
  add_test(NAME hymission-input-guard-test COMMAND hymission-input-guard-test)
  add_executable(hymission-private-debug-log-test tools/private_debug_log_test.cpp)
  target_include_directories(hymission-private-debug-log-test PRIVATE src)
  add_test(NAME hymission-private-debug-log-test COMMAND hymission-private-debug-log-test)
  set_tests_properties(hymission-private-debug-log-test PROPERTIES TIMEOUT 10)
  find_program(HYPRLAND_TEST_EXECUTABLE Hyprland)
  if(HYPRLAND_TEST_EXECUTABLE)
    add_library(hymission-stage-edge-native-probe MODULE tools/stage_edge_native_contract_probe.cpp)
    get_target_property(EDGE_NATIVE_INCLUDES hymission INCLUDE_DIRECTORIES)
    target_include_directories(hymission-stage-edge-native-probe PRIVATE src ${EDGE_NATIVE_INCLUDES})
    target_link_libraries(hymission-stage-edge-native-probe PRIVATE ${HYPR_DEPS_LIBRARIES} ${LUA_LIBRARIES} ${GLIB_LIBRARIES})
    add_test(NAME hymission-stage-edge-native-contract-test
      COMMAND ${CMAKE_COMMAND} -E env
        "LD_PRELOAD=$<TARGET_FILE:hymission-stage-edge-native-probe>"
        HYMISSION_NATIVE_FRAMEBUFFER_PROBE=1 -- ${HYPRLAND_TEST_EXECUTABLE} --version)
    set_tests_properties(hymission-stage-edge-native-contract-test PROPERTIES TIMEOUT 10
      PASS_REGULAR_EXPRESSION "PASS: Stage native GL cast, getFBID, invalidate and release")
  endif()
  pkg_check_modules(EDGE_TEST_GL QUIET IMPORTED_TARGET egl glesv2 hyprutils hyprgraphics libdrm)
  if(EDGE_TEST_GL_FOUND)
    add_executable(hymission-stage-edge-render-test tools/stage_edge_render_test.cpp)
    target_include_directories(hymission-stage-edge-render-test PRIVATE src)
    target_link_libraries(hymission-stage-edge-render-test PRIVATE PkgConfig::EDGE_TEST_GL)
    add_test(NAME hymission-stage-edge-render-test COMMAND hymission-stage-edge-render-test)
    set_tests_properties(hymission-stage-edge-render-test PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT "LIBGL_ALWAYS_SOFTWARE=1")
    if(HYPRLAND_TEST_EXECUTABLE)
      add_library(hymission-snapshot-color-test MODULE tools/snapshot_color_test.cpp)
      target_include_directories(hymission-snapshot-color-test PRIVATE src ${EDGE_NATIVE_INCLUDES})
      target_link_directories(hymission-snapshot-color-test PRIVATE ${HYPR_DEPS_LIBRARY_DIRS})
      target_link_libraries(hymission-snapshot-color-test PRIVATE PkgConfig::EDGE_TEST_GL)
      add_test(NAME hymission-snapshot-color-test
        COMMAND ${CMAKE_COMMAND} -E env
          "LD_PRELOAD=$<TARGET_FILE:hymission-snapshot-color-test>"
          HYMISSION_SNAPSHOT_COLOR_TEST=1 -- ${HYPRLAND_TEST_EXECUTABLE} --version)
      set_tests_properties(hymission-snapshot-color-test PROPERTIES TIMEOUT 15 SKIP_RETURN_CODE 77
        ENVIRONMENT "LIBGL_ALWAYS_SOFTWARE=1"
        PASS_REGULAR_EXPRESSION "PASS: snapshot HDR range, CM identity, SDR pixels, resize and failure propagation")
      foreach(libdir ${HYPR_DEPS_LIBRARY_DIRS})
        set_property(TEST hymission-snapshot-color-test APPEND PROPERTY
          ENVIRONMENT_MODIFICATION "LD_LIBRARY_PATH=path_list_prepend:${libdir}")
      endforeach()
    endif()
  endif()
  add_executable(hymission-stage-logic-test tools/stage_logic_test.cpp src/stage_logic.cpp src/mission_layout.cpp)
  target_include_directories(hymission-stage-logic-test PRIVATE src)
  add_test(NAME hymission-stage-logic-test COMMAND hymission-stage-logic-test)
  pkg_check_modules(WAYLAND_SERVER REQUIRED IMPORTED_TARGET wayland-server)
  add_executable(hymission-search-child-process-test tools/search_child_process_test.cpp)
  target_include_directories(hymission-search-child-process-test PRIVATE src)
  target_link_libraries(hymission-search-child-process-test PRIVATE PkgConfig::WAYLAND_SERVER)
  add_test(NAME hymission-search-child-process-test COMMAND hymission-search-child-process-test)
  set_tests_properties(hymission-search-child-process-test PROPERTIES TIMEOUT 10)
  add_executable(hymission-mission-layout-test
    tools/mission_layout_test.cpp
    src/mission_layout.cpp
  )

  target_include_directories(hymission-mission-layout-test PRIVATE src)
  add_test(NAME hymission-mission-layout-test COMMAND hymission-mission-layout-test)

  add_executable(hymission-overview-logic-test
    tools/overview_logic_test.cpp
    src/overview_logic.cpp
  )

  target_include_directories(hymission-overview-logic-test PRIVATE src ${HYPR_DEPS_INCLUDE_DIRS} ${GLIB_INCLUDE_DIRS})
  target_link_directories(hymission-overview-logic-test PRIVATE ${HYPR_DEPS_LIBRARY_DIRS} ${GLIB_LIBRARY_DIRS})
  target_link_libraries(hymission-overview-logic-test PRIVATE ${HYPR_DEPS_LIBRARIES} ${GLIB_LIBRARIES})
  add_test(NAME hymission-overview-logic-test COMMAND hymission-overview-logic-test)
  set_tests_properties(hymission-overview-logic-test PROPERTIES TIMEOUT 10)
endif()

install(TARGETS hymission LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(TARGETS hymission-search-input RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})
