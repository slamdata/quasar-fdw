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
BUILD=0

function ensure_requirements()
{
    docker ps >/dev/null || error "Couldn't contact docker host"
}

function build()
{
    if [[ "$BUILD" == "1" ]]; then
        docker build -f docker/mongodb.dockerfile -t local/mongodb .
        docker build -f docker/quasar.dockerfile -t local/quasar .

        docker build -f docker/ubuntu-trusty.dockerfile \
               -t local/quasar_fdw:ubuntu-trusty .
    fi
}

function setup()
{
    if [[ -z $(docker images | grep local/quasar_fdw) ]]; then
        error "No images found. Try running $0 --build"
    fi
    docker run -d --name quasar-test-mongodb local/mongodb
    docker run -d --name quasar-test-quasar \
           --link quasar-test-mongodb:mongodb local/quasar
}

function run()
{
    docker run --rm \
           --link quasar-test-quasar:quasar \
           --env QUASAR_SERVER=quasar \
           --env QUASAR_PATH=/test/quasar \
           --env QUASAR_PORT=8080 \
           -v $(pwd)/test:/app/quasar_fdw/test \
           local/quasar_fdw:ubuntu-trusty
}

function teardown()
{
    docker stop quasar-test-mongodb
    docker stop quasar-test-quasar
    docker rm quasar-test-mongodb
    docker rm quasar-test-quasar
}

function error()
{
    echo $@ >&2
    exit 127
}

function main()
{
    ensure_requirements
    build
    setup
    run
    teardown
}

function help()
{
    echo "$0 [options]"
    echo "Cross-platform testing for quasar_fdw using docker"
    echo ""
    echo "Options:"
    echo " -h|--help          Show this message"
    echo " -b|--build         Build docker containers"
    exit 1
}

while [[ $# > 0 ]]
do
    case "$1" in
        -h|--help)
            help
            ;;
        -b|--build)
            BUILD=1
            ;;
        *)
            echo "Options $1 not known."
            help
            ;;
    esac
    shift # past argument or value
done
main
