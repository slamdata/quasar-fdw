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
#        quasar_fdw/Makefile
#
##############################################################################

## Quasar Configuration

MONGO_HOST ?= localhost
MONGO_PORT ?= 27017
MONGO_DB   ?= quasar
QUASAR_DIR ?= ../quasar
SCALA_VERSION ?= 2.11
QUASAR_VERSION = $(shell cat $(QUASAR_DIR)/version.sbt | cut -d'=' -f2 | xargs)

## Quasar FDW Configuration

CURL_LIB = $(shell curl-config --libs)
MY_LIBS = $(CURL_LIB)
SHLIB_LINK = $(MY_LIBS)

## PGXS Configuration

EXTENSION    = quasar_fdw
EXTVERSION   = $(shell grep default_version $(EXTENSION).control | sed -e "s/default_version[[:space:]]*=[[:space:]]*'\([^']*\)'/\1/")

DATA         = $(filter-out $(wildcard sql/*--*.sql),$(wildcard sql/*.sql))
DOCS         = $(wildcard doc/*.md)
USE_MODULE_DB = 1
TESTS        = $(wildcard test/sql/*.sql)
REGRESS      = $(patsubst test/sql/%.sql,%,$(TESTS))
REGRESS_OPTS = --inputdir=test --outputdir=test \
	--load-language=plpgsql --load-extension=$(EXTENSION)
MODULE_big      = $(EXTENSION)
OBJS         =  $(patsubst %.c,%.o,$(wildcard src/*.c))
PG_CONFIG    = pg_config

all: sql/$(EXTENSION)--$(EXTVERSION).sql

sql/$(EXTENSION)--$(EXTVERSION).sql: sql/$(EXTENSION).sql
	cp $< $@

DATA_built = sql/$(EXTENSION)--$(EXTVERSION).sql
DATA = $(filter-out sql/$(EXTENSION)--$(EXTVERSION).sql, $(wildcard sql/*--*.sql))
EXTRA_CLEAN = sql/$(EXTENSION)--$(EXTVERSION).sql

PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# we put all the tests in a test subdir, but pgxs expects us not to, darn it
override pg_regress_clean_files = test/results/ test/regression.diffs test/regression.out tmp_check/ log/


## Integration Tests

import-test-data:
	$(QUASAR_DIR)/scripts/importTestData $(MONGO_HOST) $(MONGO_PORT) $(MONGO_DB)

build-quasar:
	cd $(QUASAR_DIR) && ./sbt 'project web' oneJar

start-quasar:
	java -jar $(QUASAR_DIR)/web/target/scala-$(SCALA_VERSION)/web_$(SCALA_VERSION)-$(QUASAR_VERSION)-one-jar.jar -c test/quasar-config.json
