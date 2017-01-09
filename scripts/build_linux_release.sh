set -e

FDWVERSION=v$(cat quasar_fdw.control | grep default_version | cut -d'=' -f2 | xargs)

function build()
{
    cat docker/build-linux.dockerfile \
        | sed "s/%%POSTGRES_VERSION%%/${1}/g" \
        | sed "s/%%DISTRO%%/${2}/g" \
              > .temp.build-linux-$1.dockerfile

    docker build -f .temp.build-linux-$1.dockerfile -t quasar_fdw_build/quasar_fdw:build-linux .
    docker run --rm quasar_fdw_build/quasar_fdw:build-linux > quasar_fdw-linux-x86_64-${1}-${FDWVERSION}.tar.gz
}

build 9.4 trusty # 9.4 isn't available on xenial
build 9.5 xenial
