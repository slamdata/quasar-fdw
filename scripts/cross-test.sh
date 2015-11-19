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
#            quasar_fdw/scripts/cross-test.sh
#
# Cross-platform testing using docker
#
#-------------------------------------------------------------------------
set -e

NO_LOCAL=
NO_DOCKER=
FORCE_BUILD=
PIDS=()
DOCKER_NS=quasar_fdw_test
DOCKER_TESTS=(ubuntu-trusty ubuntu-wily debian-jessie)

function testmsg()
{
    echo "***********************************************"
    echo "***********     $1"
    echo "***********************************************"
}

function ensure_docker_requirements()
{
    docker ps >/dev/null || error "Couldn't contact docker host"
    make clean
}

function build_docker()
{
    if [[ ! -z $FORCE_BUILD ]] || [[ -z $(docker images | grep $DOCKER_NS/mongodb) ]]; then
        docker build -f docker/mongodb.dockerfile -t $DOCKER_NS/mongodb .
    fi
    if [[ ! -z $FORCE_BUILD ]] || [[ -z $(docker images | grep $DOCKER_NS/quasar) ]]; then
        docker build -f docker/quasar.dockerfile -t $DOCKER_NS/quasar .
    fi

    for test in ${DOCKER_TESTS[@]}; do
        if [[ ! -z $FORCE_BUILD ]] || [[ -z $(docker images | grep $DOCKER_NS/quasar_fdw | grep $test) ]]; then
            echo "BUILDING $test"
            docker build -f docker/$test.dockerfile \
                   -t $DOCKER_NS/quasar_fdw:$test .
        fi
    done
}

function setup_docker()
{
    build_docker
    echo "Starting mongo and quasar docker containers"
    docker run -d --name $DOCKER_NS-mongodb $DOCKER_NS/mongodb
    sleep 1
    docker run -d --name $DOCKER_NS-quasar \
           --link $DOCKER_NS-mongodb:mongodb $DOCKER_NS/quasar
    sleep 20
}

function teardown_docker()
{
    docker stop $DOCKER_NS-mongodb 2>/dev/null || echo "" > /dev/null
    docker stop $DOCKER_NS-quasar 2>/dev/null || echo "" > /dev/null
    docker rm $DOCKER_NS-mongodb 2>/dev/null || echo "" > /dev/null
    docker rm $DOCKER_NS-quasar 2>/dev/null || echo "" > /dev/null
}

function run_docker()
{
    ensure_docker_requirements
    teardown_docker
    setup_docker
    for test in ${DOCKER_TESTS[@]}; do
        echo "RUNNING TEST $test"
        docker run -i --rm \
               --link $DOCKER_NS-quasar:quasar \
               --env QUASAR_SERVER=quasar \
               --env QUASAR_PATH=/test/quasar \
               --env QUASAR_PORT=8080 \
               -v $(pwd)/test:/app/quasar_fdw/test \
               $DOCKER_NS/quasar_fdw:$test \
            && testmsg "TEST $test: SUCCEEDED" \
            || (testmsg "TEST $test: FAILED" && exit 127)
    done
    teardown_docker
}

function run_local()
{
    echo "Ensuring postgres, quasar, and mongodb are started"
    echo "If these fail, you should start them yourself and re-run."
    if ! (mongo -eval '1' >/dev/null 2>&1); then
        echo "Starting temporary mongodb"
        nohup mongod --config /usr/local/etc/mongod.conf > /tmp/mongodb.log 2>&1 &
        PIDS+=($!)
    fi
    if ! (psql --list >/dev/null 2>&1); then
        echo "Starting temporary postgresql"
        nohup postgres -D /usr/local/var/postgres -d 1 > /tmp/postgres.log 2>&1 &
        PIDS+=($!)
    fi
    if ! (curl http://localhost:8080/query/fs/local/quasar?q=SELECT%20city%20FROM%20zips%20LIMIT%201 >/dev/null 2>&1); then
        echo "Starting temporary quasar"
        nohup make start-quasar > /tmp/quasar.log 2>&1 &
        PIDS+=($!)
    fi

    sleep 5

    make install installcheck \
        && testmsg "LOCAL TEST: SUCCEEDED" \
        || (testmsg "LOCAL TEST: FAILED" && exit 127)

    for pid in ${PIDS[@]}; do
        kill $pid
    done
}

function error()
{
    echo $@ >&2
    exit 127
}

function main()
{
    if [[ -z $NO_DOCKER ]]; then
        run_docker
        testmsg "ALL DOCKER TESTS PASS"
    fi
    if [[ -z $NO_LOCAL ]]; then
        run_local
    fi

    if [[ -z "${NO_LOCAL}${NO_DOCKER}" ]]; then
        testmsg "ALL TESTS PASS"
    fi
}

function help()
{
    echo "$0 [options]"
    echo "Cross-platform testing for quasar_fdw locally and using docker"
    echo ""
    echo "Options:"
    echo " -h|--help          Show this message"
    echo " -l|--local-only    Only test locally"
    echo " -d|--docker-only   Only test in docker"
    echo " -b|--force-build   Forcibly rebuild docker images"
    exit 1
}

while [[ $# > 0 ]]
do
    case "$1" in
        -h|--help)
            help
            ;;
        -l|--local-only)
            NO_DOCKER=1
            ;;
        -d|--docker-only)
            NO_LOCAL=1
            ;;
        -b|--force-build)
            FORCE_BUILD=1
            ;;
        *)
            echo "Options $1 not known."
            help
            ;;
    esac
    shift # past argument or value

done

main
