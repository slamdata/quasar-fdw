FDWVERSION=v$(cat quasar_fdw.control | grep default_version | cut -d'=' -f2 | xargs)

docker build -f docker/build-linux.dockerfile -t quasar_fdw_build/quasar_fdw:build-linux .
docker run --rm quasar_fdw_build/quasar_fdw:build-linux > quasar_fdw-linux-x86_64-9.4.5-${FDWVERSION}.tar.gz
