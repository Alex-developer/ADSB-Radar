set(PROJECT_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(AIRPORT_CSV ${PROJECT_ROOT}/data/airports.csv)
set(AIRPORT_GENERATED_CPP ${CMAKE_CURRENT_BINARY_DIR}/airport_data.cpp)
set(AIRPORT_GENERATED_H ${CMAKE_CURRENT_BINARY_DIR}/airport_data.h)
set(BOUNDARY_GEOJSON ${PROJECT_ROOT}/data/world.geojson)
set(BOUNDARY_GENERATED_CPP ${CMAKE_CURRENT_BINARY_DIR}/country_boundary_data.cpp)
set(BOUNDARY_GENERATED_H ${CMAKE_CURRENT_BINARY_DIR}/country_boundary_data.h)

if(NOT CMAKE_SCRIPT_MODE_FILE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    # Convert the airport CSV into link-time C arrays. Parsing a 276 KB CSV on
    # the ESP at boot would waste RAM and delay the first radar draw.
    add_custom_command(
        OUTPUT ${AIRPORT_GENERATED_CPP} ${AIRPORT_GENERATED_H}
        COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_ROOT}/tools/generate_airports.py
                ${AIRPORT_CSV}
                ${AIRPORT_GENERATED_CPP}
                ${AIRPORT_GENERATED_H}
        DEPENDS ${AIRPORT_CSV} ${PROJECT_ROOT}/tools/generate_airports.py
        VERBATIM)

    # Convert the country GeoJSON into simplified, bounded line data. The source
    # file is too large for the device to parse or keep in memory at runtime.
    add_custom_command(
        OUTPUT ${BOUNDARY_GENERATED_CPP} ${BOUNDARY_GENERATED_H}
        COMMAND ${Python3_EXECUTABLE}
                ${PROJECT_ROOT}/tools/generate_boundaries.py
                ${BOUNDARY_GEOJSON}
                ${BOUNDARY_GENERATED_CPP}
                ${BOUNDARY_GENERATED_H}
        DEPENDS ${BOUNDARY_GEOJSON} ${PROJECT_ROOT}/tools/generate_boundaries.py
        VERBATIM)

    # stb_image brings a few static helpers that are intentionally unused in
    # this JPEG-only build; silence those warnings for this source file only.
    set_source_files_properties(ui/PhotoDecoder.cpp PROPERTIES
        COMPILE_OPTIONS "-Wno-unused-function")
endif()

# Application sources are grouped by responsibility to mirror the folder layout.
set(APP_SOURCES
    app/RadarApp.cpp
    aircraft/AircraftDataService.cpp
    aircraft/AircraftFetchService.cpp
    aircraft/AircraftPhotoService.cpp
    aircraft/AircraftRouteService.cpp
    airports/AirportRunwayService.cpp
    core/RadarGeometry.cpp
    core/RadarPositionProvider.cpp
    core/RadarSettings.cpp
    gps/GpsReceiver.cpp
    net/RadarHttpClient.cpp
    settings/SettingsPageHtml.cpp
    settings/SettingsServer.cpp
    ui/AircraftPopupController.cpp
    ui/CurvedButtonController.cpp
    ui/HardwareControlService.cpp
    ui/PhotoDecoder.cpp
    ui/Ssd1306StatusDisplay.cpp
    wifi/CaptivePortal.cpp
    wifi/WifiManager.cpp)

idf_component_register(
    SRCS ${APP_SOURCES} ${AIRPORT_GENERATED_CPP} ${BOUNDARY_GENERATED_CPP}
    INCLUDE_DIRS
        .
        app
        aircraft
        airports
        core
        gps
        net
        settings
        ui
        wifi
        ${CMAKE_CURRENT_BINARY_DIR}
    REQUIRES esp_event esp_http_client esp_http_server esp_netif esp_wifi espressif__libpng espressif__usb_host_cdc_acm espressif__zlib json mbedtls nvs_flash waveshare__esp32_p4_wifi6_touch_lcd_xc)
