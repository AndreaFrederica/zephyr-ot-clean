# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(esp32s3_fan_controller)

set(GENERATED_WEB_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(GENERATED_WEB_HEADER ${GENERATED_WEB_DIR}/generated/web_assets.hpp)
set(GENERATED_WEB_SOURCE ${GENERATED_WEB_DIR}/generated/web_assets.cpp)
set(GENERATED_CURVE_HEADER ${GENERATED_WEB_DIR}/generated/default_curve_assets.hpp)
set(GENERATED_CURVE_SOURCE ${GENERATED_WEB_DIR}/generated/default_curve_assets.cpp)
set(GENERATED_CONFIG_HEADER ${GENERATED_WEB_DIR}/generated/config_assets.hpp)
set(GENERATED_CONFIG_SOURCE ${GENERATED_WEB_DIR}/generated/config_assets.cpp)

add_custom_command(
  OUTPUT ${GENERATED_WEB_HEADER} ${GENERATED_WEB_SOURCE}
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_web_assets.py
          ${GENERATED_WEB_HEADER}
          ${GENERATED_WEB_SOURCE}
          ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html web_asset_index_html
          ${CMAKE_CURRENT_SOURCE_DIR}/web/app.js web_asset_app_js
          ${CMAKE_CURRENT_SOURCE_DIR}/web/styles.css web_asset_styles_css
  DEPENDS
          ${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_web_assets.py
          ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html
          ${CMAKE_CURRENT_SOURCE_DIR}/web/app.js
          ${CMAKE_CURRENT_SOURCE_DIR}/web/styles.css
  VERBATIM
)

add_custom_command(
  OUTPUT ${GENERATED_CURVE_HEADER} ${GENERATED_CURVE_SOURCE}
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_default_curve_assets.py
          ${GENERATED_CURVE_HEADER}
          ${GENERATED_CURVE_SOURCE}
          ${CMAKE_CURRENT_SOURCE_DIR}/config/curves/adc_to_voltage.json curve_asset_adc_to_voltage
          ${CMAKE_CURRENT_SOURCE_DIR}/config/curves/voltage_to_percent.json curve_asset_voltage_to_percent
          ${CMAKE_CURRENT_SOURCE_DIR}/config/curves/percent_to_pwm.json curve_asset_percent_to_pwm
          ${CMAKE_CURRENT_SOURCE_DIR}/config/curves/percent_to_rpm.json curve_asset_percent_to_rpm
  DEPENDS
          ${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_default_curve_assets.py
          ${CMAKE_CURRENT_SOURCE_DIR}/config/curves/adc_to_voltage.json
          ${CMAKE_CURRENT_SOURCE_DIR}/config/curves/voltage_to_percent.json
          ${CMAKE_CURRENT_SOURCE_DIR}/config/curves/percent_to_pwm.json
          ${CMAKE_CURRENT_SOURCE_DIR}/config/curves/percent_to_rpm.json
  VERBATIM
)

add_custom_command(
  OUTPUT ${GENERATED_CONFIG_HEADER} ${GENERATED_CONFIG_SOURCE}
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_config_assets.py
          ${GENERATED_CONFIG_HEADER}
          ${GENERATED_CONFIG_SOURCE}
          ${CMAKE_CURRENT_SOURCE_DIR}/config/config.json config_asset_config_json
          ${CMAKE_CURRENT_SOURCE_DIR}/config/wifi.json config_asset_wifi_json
          ${CMAKE_CURRENT_SOURCE_DIR}/config/ntp.json config_asset_ntp_json
  DEPENDS
          ${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_config_assets.py
          ${CMAKE_CURRENT_SOURCE_DIR}/config/config.json
          ${CMAKE_CURRENT_SOURCE_DIR}/config/wifi.json
          ${CMAKE_CURRENT_SOURCE_DIR}/config/ntp.json
  VERBATIM
)

target_sources(app PRIVATE
  src/main.cpp
  src/core/memory_domains.cpp
  src/cli/cli_runtime.cpp
  src/cli/shell_commands.cpp
  src/cli/line_editor.cpp
        src/cli/kilo_editor.cpp
  src/cli/command_session.cpp
  src/services/fan/curve_profiles.cpp
  src/services/storage/settings_store.cpp
  src/services/fan/fan_shared_state.cpp
  src/services/fan/fan_controller.cpp
  src/services/network/wifi_manager.cpp
  src/services/network/http/http_common.cpp
  src/services/network/http/http_api.cpp
  src/services/network/http/http_server.cpp
  src/services/ssh/ssh_server.cpp
  src/services/storage/storage.cpp
  src/services/host_control/host_control_manager.cpp
  ${GENERATED_WEB_SOURCE}
  ${GENERATED_CURVE_SOURCE}
  ${GENERATED_CONFIG_SOURCE}
)

target_include_directories(app PRIVATE
  src
  src/core
  src/cli
  src/services
  src/services/fan
  src/services/network
  src/services/network/http
  src/services/storage
  src/services/ssh
  src/services/host_control
  ${GENERATED_WEB_DIR}
)
