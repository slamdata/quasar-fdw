FROM debian:jessie

WORKDIR /app/quasar_fdw

ADD scripts/bootstrap.sh /app/quasar_fdw/scripts/bootstrap.sh
RUN apt-get update && \
    apt-get install -y sudo && \
    scripts/bootstrap.sh --verbose --requirements-only --source

RUN service postgresql start && \
    sudo -u postgres createuser --superuser root

ADD . /app/quasar_fdw
RUN make all

CMD service postgresql restart && \
    make install && \
    scripts/test.sh --quasar-server http://$QUASAR_SERVER:$QUASAR_PORT \
                    --quasar-path $QUASAR_PATH
