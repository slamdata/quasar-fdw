##############################################################################
#
# Quasar Foreign Data Wrapper for PostgreSQL
#
# Copyright (c) 2015 SlamData
#
# This software is released under the PostgreSQL Licence
#
# Author: Jon Eisen <jon@joneisen.works>
#
# IDENTIFICATION
#        quasar_fdw/test/unit/Makefile
#
##############################################################################


PG_CFLAGS = -I$(shell pg_config --includedir-server) $(shell pg_config --cflags)
CFLAGS = -g $(PG_CFLAGS)
LDFLAGS = -lcurl -lcheck -lcsv $(shell pg_config --libs) $(shell pg_config --ldflags)
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

all:

test: unit-test
	./unit-test

unit-test: *.c ../../src/*.c
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@
	chmod +x $@

clean:
	rm -f unit-test

.PHONY: test clean
