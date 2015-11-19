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
VERBOSE=0
NO_FDW=

## Configuration
POSTGRES_VERSION_REGEX=9.4

## User-customizable variables
FDWVERSION=${FDWVERSION:-v0.2.1}
YAJLVERSION=${YAJLVERSION:-646b8b82ce5441db3d11b98a1049e1fcb50fe776}
FDWCLONEURL=${FDWCLONEURL:-https://github.com/yanatan16/quasar_fdw}

##
## Platform agnostic installation functions
##

function install()
{
    log "Installing for OS $OS"
    make_tempdir
    mypushd $DIR
    ensure_requirements
    test_current_postgres_install
    if [[ "$TODO_PG_INSTALL" == "1" ]] || [[ "$TODO_PG_UPDATE" == "0" ]]; then
        install_postgres
    fi
    install_builddeps
    install_yajl
    if [[ -z $NO_FDW ]]; then
        install_fdw
    fi
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
    (logx git clone $FDWCLONEURL "fdw_$FDWVERSION") \
         || error "Clone of FDW respoitory failed"
    mypushd "fdw_$FDWVERSION"
    (logx git checkout $FDWVERSION) \
        || error "Couldn't find Quasar FDW version $FDWVERSION"
    (logx make install) \
        || error "Error installing Quasar FDW"
    mypopd
}

function install_yajl()
{
    log "Installing yajl version $YAJLVERSION"
    (logx git clone https://github.com/lloyd/yajl "yajl_$YAJLVERSION") \
         || error "Clone of YAJL repository failed"
    mypushd "yajl_$YAJLVERSION"
    (logx git checkout $YAJLVERSION) \
         || error "Couldn't find yajl version $YAJLVERSION"
    (logx ./configure) \
        || error "Configuration of YAJL failed"
    (logx make clean install) \
        || error "Installation of YAJL failed"
    if [[ -z $LD_LIBRARY_PATH ]]; then
        mv /usr/local/lib/libyajl* /usr/lib/
    fi
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

function ensure_requirements()
{
    case $OS in
        osx)
            if [[ -z $(which brew) ]]; then
                error "Homebrew is required to use this script on OSX"
            fi
            ;;
        debian)
            ;;
        *)
            ;;
    esac
}

function install_builddeps()
{
    case $OS in
        osx)
            (logx brew install git make cmake) \
                 || error "Couldn't install build dependencies"
            ;;
        debian)
            (logx sudo apt-get install -y git make cmake libcurl4-openssl-dev curl) \
                || error "Couldn't install build dependencies"
            ;;
        *)
            ;;
    esac
}

function install_postgres()
{
    case $OS in
        osx)
            if [[ "$TODO_PG_UPDATE" == "1" ]]; then
                (logx brew unlink postgresql) \
                    || error "Couldn't unlink old postgres install"
            fi
            (logx brew install postgresql) \
                || error "Couldn't install postgres package $OSX_POSTGRES_PACKAGE"
            ;;
        debian)
            if [[ "$TODO_PG_UPDATE" == "1" ]]; then
                (logx sudo apt-get uninstall -y postgresql)
            fi

            (logx sudo apt-get install -y wget) \
                || error "Couldn't install wget"

            . /etc/lsb-release
            echo "deb http://apt.postgresql.org/pub/repos/apt/ $DISTRIB_CODENAME-pgdg main" \
                 > /etc/apt/sources.list.d/pgdg.list
            (logx wget --quiet -O - \
                  https://www.postgresql.org/media/keys/ACCC4CF8.asc \
                  | sudo apt-key add -) \
                || error "Couldn't get postgresql repo signing key"
            logx sudo apt-get update

            (logx sudo apt-get install -y postgresql-9.4 \
                                          postgresql-server-dev-9.4) \
                || error "Couldn't install postgresql-9.4"
            ;;
        *)
            ;;
    esac
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
    echo "$0 [options]"
    echo ""
    echo " Bootstrap Quasar FDW and PostgreSQL"
    echo "Options:"
    echo " -h|--help                Show this message"
    echo " -v|--verboase            Print a lot of stuff"
    echo " -r|--requirements-only   Don't install the FDW itself"
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
    echo "cmd: $@" | tee -a $LOG
    if [[ "$VERBOSE" == "1" ]]; then
        $@ 2>&1 | tee -a $LOG
    else
        $@ >> $LOG 2>&1
    fi
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

while [[ $# > 0 ]]
do
    case "$1" in
        -h|--help)
            help
            ;;
        -v|--verbose)
            VERBOSE=1
            ;;
        -r|--requirements-only)
            NO_FDW=1
            ;;
        "")
            ;;
        *)
            error "Unknown option. Try --help"
            ;;
    esac
    shift
done

main
