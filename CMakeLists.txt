cmake_minimum_required(VERSION 3.19)
project(hymission VERSION "0.7.1")

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
endif()

install(TARGETS hymission LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(TARGETS hymission-search-input RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})
