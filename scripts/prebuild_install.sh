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
#            quasar_fdw/scripts/prebuild_install.sh
#
# Install script for prebuilt binaries
#
#-------------------------------------------------------------------------
set -e

function log()
{
    echo "$@"
    $@
}

function install_yajl()
{
    cd yajl
    log cp -r * /usr/local
    cd ..
}

function install_quasar_fdw()
{
    cd quasar_fdw
    PGXS=$(pg_config --pgxs)
    PGXS_DIR=$(dirname $PGXS)
    INSTALL_SH="${PGXS_DIR}/../../config/install-sh"
    PKGLIBDIR=$(pg_config --pkglibdir)
    SHAREDIR="$(pg_config --sharedir)/extension"
    DOCDIR="$(pg_config --docdir)/extension"
    SQLFILES=$(ls sql/quasar_fdw--*.sql)

    log /bin/sh ${INSTALL_SH} -c -d "${PKGLIBDIR}"
    log /bin/sh ${INSTALL_SH} -c -d "${SHAREDIR}"
    log /bin/sh ${INSTALL_SH} -c -d "${DOCDIR}"

    log /usr/bin/install -c -m 755 quasar_fdw.so "${PKGLIBDIR}/quasar_fdw.so"
    log /usr/bin/install -c -m 644 quasar_fdw.control "${SHAREDIR}/"
    log /usr/bin/install -c -m 644 "$SQLFILES" "${SHAREDIR}/"
    log /usr/bin/install -c -m 644 doc/quasar_fdw.md "${DOCDIR}/"
    cd ..
}

install_yajl
install_quasar_fdw
