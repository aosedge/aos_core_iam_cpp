#
# Copyright (C) 2024 Renesas Electronics Corporation.
# Copyright (C) 2024 EPAM Systems, Inc.
#
# SPDX-License-Identifier: Apache-2.0
#

set(TARGET aos_iamanager)

# ######################################################################################################################
# Defines
# ######################################################################################################################

# ######################################################################################################################
# Includes
# ######################################################################################################################

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# ######################################################################################################################
# Modules
# ######################################################################################################################

add_subdirectory(app)
add_subdirectory(config)
add_subdirectory(database)
add_subdirectory(fileidentifier)
add_subdirectory(iamclient)
add_subdirectory(iamserver)
add_subdirectory(nodeinfoprovider)
add_subdirectory(visidentifier)

# ######################################################################################################################
# Sources
# ######################################################################################################################

set(SOURCES main.cpp)

# ######################################################################################################################
# Target
# ######################################################################################################################

add_executable(${TARGET} ${SOURCES})

# ######################################################################################################################
# Includes
# ######################################################################################################################

# ######################################################################################################################
# Compiler flags
# ######################################################################################################################

target_compile_options(${TARGET} PRIVATE -Wstack-usage=${AOS_STACK_USAGE})

# ######################################################################################################################
# Libraries
# ######################################################################################################################

target_link_libraries(${TARGET} aoscommon app)
target_link_options(${TARGET} PRIVATE -rdynamic)

# ######################################################################################################################
# Install
# ######################################################################################################################

install(TARGETS ${TARGET} RUNTIME DESTINATION bin)
