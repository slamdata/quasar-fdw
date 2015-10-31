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

CFLAGS = -Wall -Werror
LDFLAGS = -lcurl

all:

test: unit-test
	./unit-test

unit-test: unit-test.c ../../src/quasar_api.c
	$(CC) $(CFLAGS) $(LDFLAGS) unit-test.c ../../src/quasar_api.c -o $@
	chmod +x $@

clean:
	rm -f unit-test

.PHONY: test clean
