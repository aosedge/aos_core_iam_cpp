#
# Copyright (C) 2024 Renesas Electronics Corporation.
# Copyright (C) 2024 EPAM Systems, Inc.
#
# SPDX-License-Identifier: Apache-2.0
#

set(TARGET database)

# ######################################################################################################################
# Sources
# ######################################################################################################################

set(SOURCES database.cpp)

# ######################################################################################################################
# Target
# ######################################################################################################################

add_library(${TARGET} STATIC ${SOURCES})

# ######################################################################################################################
# Includes
# ######################################################################################################################

# ######################################################################################################################
# Compiler flags
# ######################################################################################################################

add_definitions(-DLOG_MODULE="database")
target_compile_options(${TARGET} PRIVATE -Wstack-usage=${AOS_STACK_USAGE})

# ######################################################################################################################
# Libraries
# ######################################################################################################################

target_link_libraries(${TARGET} PUBLIC aoscommon aosmigration aosutils Poco::DataSQLite Poco::JSON)
