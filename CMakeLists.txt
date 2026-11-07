cmake_minimum_required(VERSION 3.22)
# macOS 10.15 (Catalina) como mínimo en Intel; la parte Apple Silicon no puede pedir menos de macOS 11 (lo fija Apple y el compilador
# lo sube solo). Con 10.15 libc++ trae std::optional, std::filesystem, etc. sin trucos. Tiene que ir ANTES de project(): después CMake lo
# ignora y el programa sale pidiendo el macOS del SDK con que se compila (30 oct–7 nov 2026: salió exigiendo macOS 26).
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Versión mínima de macOS")
project(synz_puente VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(APPLE)
  enable_language(OBJC OBJCXX)
endif()

# JUCE 8: sale de una copia local si se indica JUCE_DIR; si no, se descarga la versión fijada.
# Ojo con la licencia: JUCE es AGPLv3 o comercial. Ver README.md.
set(JUCE_DIR "" CACHE PATH "Ruta a una copia de JUCE 8 (si se deja vacía, se descarga)")
if(JUCE_DIR)
  add_subdirectory(${JUCE_DIR} JUCE)
else()
  include(FetchContent)
  FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.9
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(JUCE)
endif()

option(SYNZ_PUENTE_PRUEBAS "Compila el servidor eco y los plugins de prueba" ON)

# El servidor WebSocket se compila dentro de cada programa que lo usa.
set(SYNZ_WS_FUENTES
  ${CMAKE_CURRENT_SOURCE_DIR}/fuente/ws/WsConexion.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/fuente/ws/WsServidor.cpp)
set(SYNZ_WS_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/fuente/ws)

add_subdirectory(fuente)

if(SYNZ_PUENTE_PRUEBAS)
  add_subdirectory(pruebas)
endif()
