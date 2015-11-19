FROM mongo:3.0

ADD . /app/quasar_fdw
ADD https://github.com/quasar-analytics/quasar/archive/v2.2.3-SNAPSHOT-1983-web.tar.gz /app/quasar.tar.gz

WORKDIR /app/quasar_fdw

RUN apt-get update && \
    apt-get install -y wget make procps && \
    tar -C /app -xzvf /app/quasar.tar.gz && \
    mkdir /data/mongodb && \
    chown -R mongodb:mongodb /data/mongodb

ENV QUASAR_DIR=/app/quasar-2.2.3-SNAPSHOT-1983-web

RUN mongod --dbpath /data/mongodb & \
     sleep 3 && \
     make import-test-data && \
     pkill mongod && \
     sleep 3

CMD mongod --dbpath /data/mongodb
