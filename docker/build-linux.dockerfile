FROM ubuntu:trusty

WORKDIR /app/quasar_fdw

ADD https://github.com/quasar-analytics/yajl/archive/646b8b82ce5441db3d11b98a1049e1fcb50fe776.tar.gz /app/yajl.tar.gz
ADD scripts/bootstrap.sh /app/quasar_fdw/scripts/bootstrap.sh
RUN scripts/bootstrap.sh --verbose --requirements-only --source
RUN cd /app && tar xzvf yajl.tar.gz

ENV YAJL_DIR=/app/yajl-646b8b82ce5441db3d11b98a1049e1fcb50fe776

ADD . /app/quasar_fdw
RUN make tar


CMD cat quasar_fdw-*x86*.tar.gz
