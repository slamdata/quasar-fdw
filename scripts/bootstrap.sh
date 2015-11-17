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
# Bootstrapping script for Postgres 9.4 and Quasar FDW
#
#-------------------------------------------------------------------------

## Internal variables
OS=
OSTYPE=
DIR=
LOG=
TODO_PG_UPDATE=0
TODO_PG_INSTALL=0
PGPID=
MONGOPID=
QUASARPID=
NOTEST=
FN=$(basename $0)

## Configuration
POSTGRES_VERSION_REGEX=9.4
OSX_POSTGRES_PACKAGE=postgresql
DEBIAN_POSTGRES_PACKAGE=
CENTOS_POSTGRES_PACKAGE=

## User-customizable variables
FDWVERSION=${FDWVERSION:-v0.2.1}
YAJLVERSION=${YAJLVERSION:-646b8b82ce5441db3d11b98a1049e1fcb50fe776}
FDWCLONEURL=${FDWCLONEURL:-https://github.com/quasar-analytics/quasar_fdw}

##
## Platform agnostic installation functions
##

function install()
{
    make_tempdir
    mypushd $DIR
    $OS_ensure_requrements
    test_current_postgres_install
    if [[ "$TODO_PG_INSTALL" == "1" || "$TODO_PG_UPDATE" == "0" ]]; then
        $OS_install_postgres
    fi
    $OS_install_builddeps
    install_yajl
    install_fdw
    mypopd
    cleanup
    log "Install is successful!"
    log "Try testing with the scripts/test.sh command"
}

function make_tempdir()
{
    DIR=$(mktemp -d /tmp/fdw_install.XXXXXX)
    LOG="$DIR/log"
    log "Temp directory is $DIR. Logfile is $LOG"
}

function test_current_postgres_install()
{
    if [[ -z $(which postgres) ]]; then
        log "You do not have PostgreSQL. Installing..."
        TODO_PG_INSTALL=1
    elif ! [[ $(postgres --version) =~ "$POSTGRES_VERSION_REGEX" ]]; then
        log "Your PostgreSQL install is out of date. It will be updated."
        TODO_PG_UPDATE=1
    else
        log "Your PostgreSQL install is up to date."
    fi
}

function install_fdw()
{
    log "Installing Quasar FDW version $FDWVERSION"
    (logxs git clone $FDWCLONEURL "fdw_$FDWVERSION") \
         || error "Clone of FDW respoitory failed"
    mypushd "fdw_$FDWVERSION"
    (logxs git checkout $FDWVERSION) \
        || error "Couldn't find Quasar FDW version $FDWVERSION"
    (logxs make install) \
        || error "Error installing Quasar FDW"
    mypopd
}

function install_yajl()
{
    log "Installing yajl version $YAJLVERSION"
    (logxs git clone https://github.com/lloyd/yajl "yajl_$YAJLVERSION") \
         || error "Clone of YAJL repository failed"
    mypushd "yajl_$YAJLVERSION"
    (logxs git checkout $YAJLVERSION) \
         || error "Couldn't find yajl version $YAJLVERSION"
    (logxs ./configure) \
        || error "Configuration of YAJL failed"
    (logxs make clean install) \
        || error "Installation of YAJL failed"
    mypopd
}

function cleanup()
{
    rm -rf "$DIR/fdw_$FDWVERSION"
    rm -rf "$DIR/yajl_$YAJLVERSION"
}

##
## OSX Installation functions
##

function osx_ensure_requirements()
{
    if [[ -z $(which brew) ]]; then
        error "Homebrew is required to use this script on OSX"
    fi
}

function osx_install_builddeps()
{
    brew install git make cmake libcurl \
         error "Couldn't install build dependencies"
}

function osx_install_postgres()
{
    if [[ ! -z $PG_UPDATE ]]; then
        (logx brew unlink $OSX_POSTGRES_PACKAGE) \
             || error "Couldn't unlink old postgres install"
    fi
    (logx brew install $OSX_POSTGRES_PACKAGE) \
         || error "Couldn't install postgres package $OSX_POSTGRES_PACKAGE"

    (logx brew link $OSX_POSTGRES_PACKAGE) \
         || error "Couldn't link postgres package"
}

##
## Non-installation functions
##

function main()
{
    figure_out_os
    case $OS in
        osx|debian|centos)
            install
            ;;
        *)
            error "OS $OS Not supported"
            ;;
    esac
}

function figure_out_os()
{
    case $(uname) in
        Darwin)
            OS=osx
            ;;
        Linux)
            if [[ ! -z $(which apt-get) ]]; then
                OS=debian
            elif [[ ! -z $(which yum) ]]; then
                OS=centos
            fi
            ;;
        *)
            error "OS $(uname) not supported at this time."
            ;;
    esac
}


function help()
{
    echo "$0"
    echo ""
    echo " Bootstrap Quasar FDW and PostgreSQL"
    echo "Env Vars:"
    echo " FDWVERSION:  Quasar FDW Git Reference (Default: master)"
    echo " YAJLVERSION: Yajl Git Reference (Default: yajl_reset)"
    exit 1
}

function error()
{
    if [[ ! -z "$@" ]]; then
        log "$@"
    else
        log "Unknown error"
    fi
    log "A full log was recorded: $LOG"

    # Escape our pushd's
    mypopdall

    exit 127
}

function log()
{
    echo "$FN: $1" | tee -a $LOG
}

# Execute a command and log output
function logx()
{
    echo "cmd: $@" >> $LOG
    $@ 2>&1 | tee -a $LOG
}

# Execute a command and log output but not to console
function logxs()
{
    echo "cmd: $@" >> $LOG
    $@ >> $LOG 2>&1
}

DIRLEVELS=0
function mypushd()
{
    pushd $1 >> $LOG
    DIRLEVELS=$(( $DIRLEVELS + 1 ))
}

function mypopd()
{
    popd >> $LOG
    DIRLEVELS=$(( $DIRLEVELS - 1 ))
}

function mypopdall()
{
    while [ "$DIRLEVELS" -gt 0 ]; do
        mypopd
    done
}

case "$1" in
    -h|--help)
        help
        ;;
    "")
        ;;
    *)
        error "Unknown option. Try --help"
        ;;
esac

main
