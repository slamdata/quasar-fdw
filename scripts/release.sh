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

# Test for docker
docker ps > /dev/null

# Test for make
make clean

# Replace tag in .control file
sed "s/default_version = .*/default_version = '$1'/" -i quasar_fdw.control

# Replace tag in bootstrap.sh
sed "s/FDWVERSION=.*/FDWVERSION=\${FDWVERSION:-v${1}}/" -i scripts/bootstrap.sh

# Replace tag in README
sed "s/Lastest Version:.*/Latest Version: \`v${1}\`/" -i README.md

# build osx tar
scripts/build_osx_release.sh

# Make linux tar
scripts/build_linux_release.sh

git add .
