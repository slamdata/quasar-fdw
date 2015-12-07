FROM java:8

WORKDIR /app
ADD docker/quasar-config.json quasar-config.json

RUN apt-get update && \
    apt-get install -y wget && \
    wget https://github.com/quasar-analytics/quasar/releases/download/v2.3.3-SNAPSHOT-2126-web/web_2.11-2.3.3-SNAPSHOT-one-jar.jar

EXPOSE 8080

CMD java -jar web_2.11-2.2.3-SNAPSHOT-one-jar.jar -c /app/quasar-config.json
