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
set -e

## Internal variables
OS=
OSTYPE=
ARCH=
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
USE_SOURCE=

## Configuration
POSTGRES_VERSION_REGEX=9.4

## User-customizable variables
FDWVERSION=${FDWVERSION:-v1.0-rc3}
YAJLCLONEURL=${YAJLCLONEURL:-https://github.com/quasar-analytics/yajl}
YAJLVERSION=${YAJLVERSION:-646b8b82ce5441db3d11b98a1049e1fcb50fe776}
FDWCLONEURL=${FDWCLONEURL:-https://github.com/quasar-analytics/quasar-fdw}

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
    if [[ "$TODO_PG_INSTALL" == "1" ]] || [[ "$TODO_PG_UPDATE" == "1" ]]; then
        install_postgres
    fi
    if [[ -z "$USE_SOURCE" ]]; then can_use_binaries; fi
    if [[ ! -z "$USE_SOURCE" ]]; then
        install_builddeps
        install_yajl
        if [[ -z $NO_FDW ]]; then
            install_fdw
        fi
    else
        install_binaries
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
    if [[ -z $(which pg_config 2>/dev/null) ]]; then
        log "You do not have PostgreSQL. Installing..."
        TODO_PG_INSTALL=1
    elif ! [[ $(pg_config --version) =~ "$POSTGRES_VERSION_REGEX" ]]; then
        log "Your PostgreSQL install is out of date. It will be updated."
        TODO_PG_UPDATE=1
    else
        log "Your PostgreSQL install is up to date."
    fi
}

function can_use_binaries()
{
    PGVERSION=$(pg_config --version | cut -d' ' -f2)
    TARBASE="quasar_fdw-${ARCH}-${PGVERSION}-${FDWVERSION}"
    TAR="${FDWCLONEURL}/releases/download/${FDWVERSION}/${TARBASE}.tar.gz"
    log "Querying for binaries: $TAR"
    if [[ -z $(curl $TAR -XHEAD --head 2>/dev/null | grep "302 Found") ]]; then
        USE_SOURCE=1
    fi
}

function install_binaries()
{
    logx wget $TAR -O ${TARBASE}.tar.gz
    logx tar xzvf ${TARBASE}.tar.gz
    pushd ${TARBASE}
    logx ./install.sh
    popd
}

function install_fdw()
{
    log "Installing Quasar FDW version $FDWVERSION"
    (logx wget "$FDWCLONEURL/archive/$FDWVERSION.tar.gz" -O "quasar_fdw-$FDWVERSION.tar.gz") \
        || error "Getting FDW repository failed"
    (logx tar xzvf "quasar_fdw-$FDWVERSION.tar.gz") \
        || error "Untaring FDW repository failed"
    mypushd "quasar_fdw-*"
    (logx make install) \
        || error "Error installing Quasar FDW"
    mypopd
}

function install_yajl()
{
    log "Installing yajl version $YAJLVERSION"
    (logx wget "$YAJLCLONEURL/archive/$YAJLVERSION.tar.gz" -O "yajl-$YAJLVERSION.tar.gz") \
        || error "Getting YAJL repository failed"
    (logx tar xzvf "yajl-$YAJLVERSION.tar.gz") \
        || error "Untaring YAJL repository failed"
    mypushd "yajl-*"
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
    if [[ -z "$USE_SOURCE" ]]; then
        rm -rf "$DIR/quasar_fdw*"
    else
        rm -rf "$DIR/quasar_fdw-$FDWVERSION"
        rm -rf "$DIR/yajl-$YAJLVERSION"
    fi
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
        *)
            ;;
    esac
}

function install_builddeps()
{
    case $OS in
        osx)
            if [[ -z $(which make) ]]; then logx brew install make; fi
            if [[ -z $(which cmake) ]]; then logx brew install cmake; fi
            ;;
        debian)
            (logx sudo apt-get install -y make cmake libcurl4-openssl-dev curl) \
                || error "Couldn't install build dependencies"
            ;;
        centos)
            (logx sudo yum install -y make cmake libcurl-devel curl gcc) \
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

            echo "deb http://apt.postgresql.org/pub/repos/apt/ ${OSVERSION}-pgdg main" \
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
        centos)
            case $OSTYPE in
                fedora)
                    logx sudo sed 's/\(\[fedora\]\)/\1\nexclude=postgresql*/' -i /etc/yum.repos.d/fedora.repo
                    logx sudo sed 's/\(\[updates\]\)/\1\nexclude=postgresql*/' -i /etc/yum.repos.d/fedora-updates.repo
                    logx sudo rpm -Uvh http://yum.postgresql.org/9.4/fedora/fedora-${OSVERSION}-x86_64/pgdg-fedora94-9.4-4.noarch.rpm
                    ;;
                centos)
                    logx sudo sed 's/\(\[base\]\)/\1\nexclude=postgresql*/' -i /etc/yum.repos.d/CentOS-Base.repo
                    logx sudo sed 's/\(\[updates\]\)/\1\nexclude=postgresql*/' -i /etc/yum.repos.d/CentOS-Base.repo
                    logx sudo rpm -Uvh http://yum.postgresql.org/9.4/redhat/rhel-${OSVERSION}-x86_64/pgdg-centos94-9.4-2.noarch.rpm
                    ;;
                rhel)
                    logx sudo sed 's/\(\[main\]\)/\1\nexclude=postgresql*/' -i /etc/yum/pluginconf.d/rhnplugin.conf
                    logx sudo sed 's/\(\[main\]\)/\1\nexclude=postgresql*/' -i /etc/yum.repos.d/fedora-updates.repo
                    logx sudo rpm -Uvh http://yum.postgresql.org/9.4/redhat/rhel-${OSVERSION}-x86_64/pgdg-redhat94-9.4-2.noarch.rpm
                    ;;
            esac

            logx sudo $YUM install -y postgresql94 postgresql94-server postgresql94-libs
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
    ARCH="$(uname | tr '[:upper:]' '[:lower:]')-$(uname -m)"

    case $(uname) in
        Darwin)
            OS=osx
            ;;
        Linux)
            if [[ -e /etc/lsb-release ]]; then
                . /etc/lsb-release
                OS=debian
                OSTYPE=ubuntu
                OSVERSION=$DISTRIB_CODENAME
            elif [[ -e /etc/os-release ]]; then
                . /etc/os-release
                case $ID in
                    debian)
                        OS=debian
                        OSTYPE=debian
                        OSVERSION=$(echo $VERSION | cut -d'(' -f2 | cut -d')' -f1)
                        ;;
                    fedora)
                        OS=centos
                        OSTYPE=fedora
                        OSVERSION=$VERSION_ID
                        ;;
                    rhel)
                        OS=centos
                        OSTYPE=rhel
                        OSVERSION=$VERSION_ID
                        ;;
                    centos)
                        OS=centos
                        OSTYPE=rhel
                        OSVERSION=$VERSION_ID
                        ;;
                    *)
                        error "OS $ID is not supported at this time."
                        ;;
                esac
                if [[ -z $(which dnf 2>/dev/null) ]]; then
                    YUM=yum
                else
                    YUM=dnf
                fi
            else
                error "This script doesn't support your OS type."
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
    echo " -s|--source              Force install from source"
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
        return $?
    else
        $@ >> $LOG 2>&1
        return $?
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
        -s|--source)
            USE_SOURCE=1
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
