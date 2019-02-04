#
# If not stated otherwise in this file or this component's LICENSE file the
# following copyright and licenses apply:
#
# Copyright 2018 Liberty Global B.V.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

cmake_minimum_required(VERSION 3.3)

set(SONAME "gdbus-client")

project(${SONAME} VERSION 1.0.0 LANGUAGES CXX)

find_package(PkgConfig)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(${SONAME} SHARED src/GDBusClient.cpp src/GDBusConverters.cpp)

set_target_properties(
    ${SONAME}     PROPERTIES
    VERSION       ${PROJECT_VERSION}
    SOVERSION     ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    PUBLIC_HEADER src/GDBusClient.hpp
)


# Use rdkloggers for logging
option(RDK_LOGGER_ENABLED "Use RDK loggers for logging" OFF)

if (${RDK_LOGGER_ENABLED})
    set(RDK_LOGGER_LIBRARIES "rdkloggers" CACHE STRING "")
    set(RDK_LOGGER_CFLAGS_OTHER "-DRDK_LOGGER_ENABLED")
    find_path(RDK_LOGGER_INCLUDE_DIRS "rdk_debug.h" PATH_SUFFIXES "include")
    message(STARUS "Using RDK logger for logging (header: ${RDK_LOGGER_INCLUDE_DIRS})")
endif()


# pkg-config based libraries
pkg_search_module(GIO      REQUIRED "gio-2.0")
pkg_search_module(GLIB     REQUIRED "glib-2.0")
pkg_search_module(GOBJECT  REQUIRED "gobject-2.0")

target_compile_options(${SONAME} PRIVATE
                ${GIO_CFLAGS_OTHER}
                ${GLIB_CFLAGS_OTHER}
                ${GOBJECT_CFLAGS_OTHER}
                ${RDK_LOGGER_CFLAGS_OTHER}
)

target_include_directories(${SONAME} PRIVATE
                ${GIO_INCLUDE_DIRS}
                ${GLIB_INCLUDE_DIRS}
                ${GOBJECT_INCLUDE_DIRS}
                ${RDK_LOGGER_INCLUDE_DIRS}
)

link_directories(${SONAME}
                ${GIO_LIBRARY_DIRS}
                ${GLIB_LIBRARY_DIRS}
                ${GOBJECT_LIBRARY_DIRS}
                ${RDK_LOGGER_LIBRARY_DIRS}
)

target_link_libraries(${SONAME} PRIVATE
                ${GIO_LIBRARIES}
                ${GLIB_LIBRARIES}
                ${GOBJECT_LIBRARIES}
                ${RDK_LOGGER_LIBRARIES}
)


# Metadata for generating .pc file
set(PKGCONF_REQ_PUB glib-2.0)

foreach(LIB ${CMAKE_CXX_IMPLICIT_LINK_LIBRARIES} ${PLATFORM_LIBS})
    set(PKGCONF_LIBS_PRIV "${PKGCONF_LIBS_PRIV} -l${LIB}")
endforeach()

configure_file(${SONAME}.pc.in ${SONAME}.pc @ONLY)


# Install artefacts
install(
        TARGETS                   ${SONAME}
        LIBRARY       DESTINATION ${CMAKE_INSTALL_LIBDIR}
        PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/${SONAME}
)

install(
        FILES       ${CMAKE_BINARY_DIR}/${SONAME}.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)
