# @@@LICENSE
#
# Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
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
# LICENSE@@@

cmake_minimum_required(VERSION 2.8.7)

project(luneos-print-adapter C)

include(webOS/webOS)
webos_modules_init(1 0 0 QUALIFIER RC2)
webos_component(0 1 0)

include(FindPkgConfig)

pkg_check_modules(GLIB2 REQUIRED glib-2.0)
include_directories(${GLIB2_INCLUDE_DIRS})
webos_add_compiler_flags(ALL ${GLIB2_CFLAGS})

pkg_check_modules(LUNASERVICE2 REQUIRED luna-service2)
include_directories(${LUNASERVICE2_INCLUDE_DIRS})
webos_add_compiler_flags(ALL ${LUNASERVICE2_CFLAGS})

pkg_check_modules(PBNJSON_C REQUIRED pbnjson_c)
include_directories(${PBNJSON_C_INCLUDE_DIRS})
webos_add_compiler_flags(ALL ${PBNJSON_C_CFLAGS_OTHER})

# CUPS is the print engine: discovery (it browses mDNS itself), the filter
# chain, queueing and retry. This daemon is only the LS2 translation surface,
# which is why there is no PDL/raster code here at all - unlike the original,
# which shipped HP's closed wprint plugins.
pkg_check_modules(CUPS cups)
if(CUPS_FOUND)
    include_directories(${CUPS_INCLUDE_DIRS})
    webos_add_compiler_flags(ALL ${CUPS_CFLAGS})
    set(CUPS_LIBS ${CUPS_LDFLAGS})
else()
    # Older CUPS ships cups-config rather than a .pc file.
    find_program(CUPS_CONFIG cups-config)
    if(NOT CUPS_CONFIG)
        message(FATAL_ERROR "CUPS not found: no cups.pc and no cups-config")
    endif()
    execute_process(COMMAND ${CUPS_CONFIG} --cflags
                    OUTPUT_VARIABLE CUPS_CFLAGS_RAW
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${CUPS_CONFIG} --libs
                    OUTPUT_VARIABLE CUPS_LIBS
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    separate_arguments(CUPS_LIBS)
    webos_add_compiler_flags(ALL ${CUPS_CFLAGS_RAW})
endif()

include_directories(src)
file(GLOB SOURCE_FILES src/*.c)

webos_add_compiler_flags(ALL -Wall)
webos_add_linker_options(ALL --no-undefined)

add_executable(luneos-print-adapter ${SOURCE_FILES})
target_link_libraries(luneos-print-adapter
    ${GLIB2_LDFLAGS} ${LUNASERVICE2_LDFLAGS} ${PBNJSON_C_LDFLAGS}
    ${CUPS_LIBS}
    m rt pthread)

webos_build_daemon()
webos_build_system_bus_files()
