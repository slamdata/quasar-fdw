#!/usr/bin/env bash
#-------------------------------------------------------------------------
#
# Quasar Foreign Data Wrapper for PostgreSQL
#
# Copyright (c) 2015 SlamData
#
# This software is released under the Apache 2 License
#
# Author: Jon Eisen <jon@joneisen.works>
#
# IDENTIFICATION
#            quasar_fdw/scripts/install.sh
#
# Release script
#
#-------------------------------------------------------------------------
set -e

if [[ -z $1 ]]; then
    echo "Usage: $0 <new-release-tag>"
    echo "Example: $0 1.1.2"
    exit 1
fi

# Replace tag in .control file
sed "s/default_version = .*/default_version = '$1'" quasar_fdw.control
