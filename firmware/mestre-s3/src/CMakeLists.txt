# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources
    ${CMAKE_SOURCE_DIR}/src/gerenciadores/*.cpp
    ${CMAKE_SOURCE_DIR}/src/dispositivos/*.cpp
    ${CMAKE_SOURCE_DIR}/src/servicos/*.cpp)
LIST(APPEND app_sources ${CMAKE_SOURCE_DIR}/src/main.cpp)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS
        "../include"
        "../../../compartilhado/protocolos"
        "../../../compartilhado/telemetria"
        "../../../compartilhado/comum")
