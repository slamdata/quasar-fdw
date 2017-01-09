#!/bin/bash -xe

brew unlink postgresql-9.4
brew unlink postgresql-9.5
brew unlink postgresql-9.6

FDWVERSION=v$(cat quasar_fdw.control | grep default_version | cut -d'=' -f2 | xargs)


function build()
{
    echo "Building for pg $1"
    brew link --force --overwrite postgresql-$1
    make clean tar
    mv quasar_fdw-darwin-x86_64-$1.*-${FDWVERSION}.tar.gz quasar_fdw-darwin-x86_64-$1-${FDWVERSION}.tar.gz
    brew unlink postgresql-$1
}

build 9.4
build 9.5
