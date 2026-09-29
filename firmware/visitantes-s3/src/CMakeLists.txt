# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)
LIST(APPEND app_sources
    ${CMAKE_SOURCE_DIR}/../compartilhado/ota/servico_ota_satelite.c
    ${CMAKE_SOURCE_DIR}/../compartilhado/comandos/servico_comandos_satelite.c)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS
        "../include"
        "../../compartilhado/protocolos"
        "../../compartilhado/telemetria"
        "../../compartilhado/comum"
        "../../compartilhado/ota"
        "../../compartilhado/comandos"
    PRIV_REQUIRES
        app_update
        spi_flash)
