FROM ubuntu:trusty

ADD . /app/quasar_fdw
WORKDIR /app/quasar_fdw
RUN scripts/bootstrap.sh --verbose && \
    service postgresql start && \
    sudo -u postgres createuser --superuser root
    
CMD service postgresql restart && \
    scripts/test.sh --quasar-server $QUASAR_SERVER \
                    --quasar-path $QUASAR_PATH
