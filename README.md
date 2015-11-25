# Quasar Foreign Data Wrapper for PostgreSQL

This FDW forwards SELECT statements to [Quasar](https://github.com/quasar-analytics/quasar).

The main advantage of using this FDW over alternatives is that it takes full advantage of the Quasar query engine by "pushing down" as many clauses from the PostgreSQL query to Quasar as possible. This includes WHERE, ORDER BY, and JOIN clauses.

## Install

For development installation, see [Development](#development).

This version of quasar_fdw requires PostgreSQL 9.4.

The `scripts/bootstrap.sh` will bootstrap the installation for you. It will install PostgreSQL 9.4 if it is not already installed. Then, if your system has precompiled binaries available, it will simply download those. Otherwise, it will download two libraries (a required json parser [yajl](https://github.com/lloyd/yajl) and this repository) and install both.

```bash
scripts/bootstrap.sh
```

Now you can create servers and tables, see [Usage](#usage).

### Verifying Installation

For testing during development see [Development Testing](#development-testing).

You can test an installation by using the `scripts/test.sh` script.

```
$ scripts/test.sh --help
scripts/test.sh [OPTIONS...]
Test an installation of quasar_fdw with local test files

Options:
 --quasar-server SERV     Quasar Server URL (default http://localhost:8080)
 --quasar-path PATH       Quasar Test Data Path (default /test)
 --pg-host HOST           PostgreSQL Host (defaults to local socket)
 --pg-port PORT           PostgreSQL Port (default to 5432)
 --pg-user USER           PostgreSQL User (default to jon)
 --fdw-version VERS       Version of FDW to test (default to current)
 ```

In order to run a test, you'll need a Quasar running with the test data set that comes with quasar installed at some path. You'll also need a PostgreSQL server running.

## Usage

### Options

The following parameters can be set on a Quasar foreign server object:

- `server`: URL of remote Quasar Server. Defaults to `http://localhost:8080`
- `path`: Path to the test data on remote Quasar. Defaults to `/test`
- `timeout_ms`: Timeout in milliseconds of querying data from Quasar. Defaults to `1000` (1s)
- `use_remote_estimate`: Boolean (`true` or `false`) to allow quasar_fdw to contact Quasar with rowcounts to estimate cost of queries. Defaults to `true`
- `fdw_startup_cost`: Cost (floating-point) of starting up a query to Quasar. Defaults to `100.0`
- `fdw_tuple_cost`: Cost (floating-point) of processing a tuple in quasar_fdw. Defaults to `0.01`

The following parameters can be set on a Quasar foreign table object:

- `table`: Name of the Quasar table / mongo collection to query. Required.
- `use_remote_estimate`: Boolean (`true` or `false`) to override the server-level option. Defaults to server's value.

The following parameters can be set on a column in a Quasar foreign table:

- `map`: The name of the column to query in Quasar. Defaults to the lowercase name of the column in PostgreSQL.
- `nopushdown`: Boolean (`true` or `false`) value telling quasar_fdw not to push down any comparison clauses with this column in it. Used when underlying data is not stored as the correct type. Defaults to `false`
- `join_rowcount_estimate`: Integer value representing the "distinctness" of a columns value in the underlying data. This will be used to estimate the number of rows that might be queried from a single value. For columns with unique values, this should be `1`. Only used in join clauses. Defaults to `1`.

### Queries

```sql

-- load extension first time after install

CREATE EXTENSION quasar_fdw;

-- load a certain version of extension

CREATE EXTENSION quasar_fdw WITH VERSION 'version';

-- create server object

CREATE SERVER quasar FOREIGN DATA WRAPPER quasar_fdw
       OPTIONS (server 'http://localhost:8080'
               ,path '/local/quasar'
               ,timeout_ms '1000'
               ,use_remote_estimate 'true'
               ,fdw_startup_cost '10'
               ,fdw_tuple_cost '0.01');

-- create foreign table

CREATE FOREIGN TABLE zips(
        city varchar,
        pop integer,
        state char(2),
        loc float[2])
    SERVER quasar
    OPTIONS (table 'zips');


-- create foreign table using column mappings

CREATE FOREIGN TABLE user_comments(
       user_id integer OPTIONS (map 'userId'),
       profile_name varchar OPTIONS (map 'profile.name'),
       age integer OPTIONS (map 'profile.age'),
       title varchar OPTIONS (map 'profile.title'),
       comment_id char(10) OPTIONS (map 'comments[*].id'),
       comment_text varchar OPTIONS (map 'comments[*].text'),
       comment_reply_to_profile integer OPTIONS (map 'comments[*].replyTo[0]'),
       comment_reply_to_comment char(10) OPTIONS (map 'comments[*].replyTo[1]'),
       comment_time date OPTIONS (map 'comments[*]."time"'))
    SERVER quasar
        OPTIONS (table 'user_comments');

-- select from table

SELECT * FROM zips LIMIT 10;

SELECT city, pop FROM zips WHERE pop % 2 = 1 LIMIT 10;

SELECT loc[1] AS lat, loc[2] AS long FROM zips LIMIT 10;

SELECT * FROM zips ORDER BY pop DESC LIMIT 10;

SELECT * FROM zips z1 INNER JOIN zips z2 ON z1.city = z2.city LIMIT 10;

-- See the query that quasar_fdw sends to Quasar

EXPLAIN (COSTS off) SELECT * FROM zips LIMIT 10;

-- See the query that Quasar sends to MongoDB

EXPLAIN (COSTS off, VERBOSE on) SELECT * FROM zips LIMIT 10;
```

### Supported Types

- string types: char, text, varchar, bpchar, name
- number types: numeric, int4, int8, int2, float4, float8, numeric, oid
- time types: time, timestamp, date, timestamptz, date
- bool
- complexy types: arrays, json, jsonb

### Gotchyas

- Postgres will downcase all field names, so if a field has a capital letter in it, you must use the map option: `OPTIONS (map "camelCaseSensitive")`
- This FDW will convert strings to other types, such as dates, times, timestamps, intervals, integers, and floats. However, if the underlying data is a string, we should _NOT_ push down type-specific operations such as WHERE clauses to Quasar. Therefore, you should enforce a no pushdown restriction in the column options. Use the `OPTIONS (nopushdown 'true')` option to force no pushdown of any clause containing the column.
- JOINs can be executed in one of three ways, depending on the cost estimation. This is why `use_remote_estimate` is so important. A merge join is used for very large and similarly sized datasets. A hash join is used for a large and a small dataset. A parameterized join is used when one join condition is only going to return a very small number of rows. This parameterized join is the best pushdown that can be achieved with PostgreSQL 9.4's FDW interface. See [test/expected/join.out](test/expected/join.out) for some examples.

## Development

### Requirements

You'll need make and a full install of PostgreSQL 9.4, as well as [yajl](https://github.com/lloyd/yajl). The `scripts/bootstrap.sh` should be able to provide this for you.

We use a fork of yajl at https://github.com/quasar-analytics/yajl, and the branch [yajl_reset](https://github.com/quasar-analytics/yajl/tree/yajl_reset).

### Development Testing

Setup:

1. Start mongodb
2. Import test data (`QUASAR_DIR=../quasar make import-test-data`). Make sure you run this command because extra test data (over the default set) is imported for date testing.
3. Start Quasar (`QUASAR_DIR=../quasar make build-quasar start-quasar`)
4. Start Postgres (`postgres -D /usr/local/var/postgres -d 1`)

And to test:

```
make install installcheck
```

#### Cross-Platform Testing

There are some dockerfiles in `docker/` which contain the setup to do cross-platform testing. It takes a while to execute, but you can run it with:

```bash
scripts/cross-test.sh
```

This will build the 5 docker images if necessary, start up the mongodb and quasar containers, then iteratively run the test images. Its a good way to test in different OSes.

### Adding more pushdown features

If [quasar](https://github.com/quasar-analytics/quasar) adds operators, it would be good to update this FDW to support pushdown of that operator. This can be done in the [quasar_query.c](src/quasar_query.c) file:

- For operators like math (+ - / ^ ...) or regex (~~ LIKE ...)
  - inside the `quasar_has_op` function
  - add the new operator to the big `if` statement
  - if the operator needs to be transformed to quasar syntax, do that inside the `if`
  - If the operator only maps to a quasar function, you can set `*asfunc` to `true`, like the `||` operator.
- For scalar array operators like `ANY` or `ALL`
    - go to the `quasar_has_scalar_array_op` function
    - You'll have to implement the mapping outright. Currently only `IN` (`ANY =`) and `NOT IN` (`ALL <>`) are implemented.
- For functions like math (power) or string (capitalize)
  - inside `quasar_has_function` function
  - add the new function to the big `if` statement
  - if the function needs to be transformed, do it inside the `if`.
- Quasar (as of 2.2.3) doesn't have support for some constants such as intervals with years and months, or NaN
    - Because PostgreSQL supports these constants, they must be filtered outer
    - This can be changed in the `quasar_has_const` function

### Releasing

```bash
scripts/release.sh <release version, eg 1.1.2>
```

This will replace the release version in the two places it goes (`quasar_fdw.control` and `scripts/bootstrap.sh`). Then it will build a tar with `make tar` for your local setup, then it will build a linux release with `scripts/build_linux_release.sh`, which uses docker.

## Legal

Copyright &copy; 2015 SlamData Inc.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

### Contributors

Contributors to the project must agree to the [Contributor License Agreement](CLA.md).
