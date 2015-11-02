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

CFLAGS = -Wall -Werror -g
LDFLAGS = -lcurl -lcheck -lcsv

all:

test: unit-test
	./unit-test

unit-test: *.c ../../src/array.c ../../src/quasar_api.c
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@
	chmod +x $@

clean:
	rm -f unit-test

.PHONY: test clean
