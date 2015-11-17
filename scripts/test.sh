#!/usr/bin/env bash
#-------------------------------------------------------------------------
#
# Quasar Foreign Data Wrapper for PostgreSQL
#
# Copyright (c) 2015 SlamData Inc
#
# This software is released under the Apache 2 License
#
# Author: Jon Eisen <jon@joneisen.works>
#
# IDENTIFICATION
#            quasar_fdw/scripts/install.sh
#
# Test script for Postgres 9.4 and Quasar FDW
#
#-------------------------------------------------------------------------

QUASAR_SERVER="http://localhost:8080"
QUASAR_PATH="/test"
POSTGRES_OPTS=
DB=test_quasar_fdw
QUASAR_DIR="$(dirname $BASH_SOURCE)/.."
TEST_DIR="$QUASAR_DIR/test"
FDW_VERSION=0.1


function error()
{
    echo $1 >&2
    exit 127
}

function test_requirements()
{
    # We need pgxs and pg_regress
    if [[ -z $(which pg_config) ]]; then
        error "pg_config isn't installed. We need a full postgres install to test"
    fi
    PGXS=$(pg_config --pgxs)
    PG_REGRESS="$(dirname $PGXS)/../test/regress/pg_regress"
    if [[ ! -e $PG_REGRESS ]]; then
        error "pg_regress isn't installed. We need a full postgres install to test."
    fi

    # Postgres must be running
    (psql ${POSTGRES_OPTS[@]} --list) >/dev/null \
        || error "Could not connect to PostgreSQL. Check options."

    # need curl
    if [[ -z $(which curl) ]]; then
        error "Could not find curl. Please install it"
    fi

    # Quasar must be running
    (curl "${QUASAR_SERVER}/welcome" > /dev/null 2>&1) \
        || error "Could not connect to Quasar. Check options."

    # Quasar must have test data in correct path
    resp=$(curl "${QUASAR_SERVER}/query/fs${QUASAR_PATH}?q=SELECT%20city%20FROM%20smallZips%20LIMIT%201" 2>/dev/null | sed 's/\r$//')
    exp='{ "city": "BELCHERTOWN" }'

    if [[ "$resp" != "$exp" ]]; then
        error "The quasar path did not contain the test data set. Check options."
    fi
}

function run_regression()
{
    PGXS=$(pg_config --pgxs)
    PG_REGRESS="$(dirname $PGXS)/../test/regress/pg_regress"
    TEST_FILES=$(ls "$TEST_DIR/sql" | sed 's/\.sql//' | xargs)

    $PG_REGRESS ${POSTGRES_OPTS[@]} \
                --db=$DB \
                --outputdir=$TEST_DIR \
                --inputdir=$TEST_DIR \
                --load-extension=quasar_fdw \
                --load-language=plpgsql \
                $TEST_FILES
}

function create_testfiles()
{
    TMPDIR=$(mktemp -d /tmp/test_quasar_fdw.XXXXXX)
    cp -r $TEST_DIR/sql $TEST_DIR/expected $TMPDIR
    cat $QUASAR_DIR/sql/create_server.sql | \
        sed "s#%%QUASAR_SERVER%%#$QUASAR_SERVER#" | \
        sed "s#%%QUASAR_PATH%%#$QUASAR_PATH#" > \
            $TMPDIR/sql/0_server.sql

    cp $TMPDIR/sql/0_server.sql $TMPDIR/expected/0_server.out

    TEST_DIR=$TMPDIR
}

function main()
{
    create_testfiles
    test_requirements
    run_regression
}

function help()
{
    echo "$0 [OPTIONS...]"
    echo "Test an installation of quasar_fdw with local test files"
    echo ""
    echo "Options:"
    echo " --quasar-server SERV     Quasar Server URL (default http://localhost:8080)"
    echo " --quasar-path PATH       Quasar Test Data Path (default /test)"
    echo " --pg-host HOST           PostgreSQL Host"
    echo " --pg-port PORT           PostgreSQL Port"
    echo " --pg-user USER           PostgreSQL User"
    echo " --fdw-version VERS       Version of FDW to test"

    exit 1
}

while [[ $# > 0 ]]
do
    case "$1" in
        --quasar-server)
            QUASAR_SERVER="$2"
            shift
            ;;
        --quasar-path)
            QUASAR_PATH="$2"
            shift
            ;;
        --pg-host)
            POSTGRES_OPTS+=("--host=$2")
            shift
            ;;
        --pg-port)
            POSTGRES_OPTS+=("--port=$2")
            shift
            ;;
        --pg-user)
            POSTGRES_OPTS+=("--username=$2")
            shift
            ;;
        --fdw-version)
            FDW_VERSION=$2
            shift
            ;;
        -h|--help)
            help
            ;;
        *)
            echo "Options $1 not known."
            help
            ;;
    esac
    shift # past argument or value
done
main
