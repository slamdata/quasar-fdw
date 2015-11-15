# Quasar Foreign Data Wrapper for PostgreSQL

This FDW forwards SELECT statements to [Quasar](https://github.com/quasar-analytics/quasar).


The main advantage of using this FDW over alternatives is that it takes full advantage of the Quasar query engine by "pushing down" as many clauses from the PostgreSQL query to Quasar as possible. This includes WHERE, ORDER BY, and JOIN clauses.

## Usage

```sql
CREATE SERVER quasar
    FOREIGN DATA WRAPPER quasar_fdw
    OPTIONS (server 'http://localhost:8080'
            ,path '/local/quasar'
            ,timeout_ms 1000);

CREATE FOREIGN TABLE zips(city varchar, loc float[2], pop integer, state char(2))
    SERVER quasar
    OPTIONS (table 'zips');

CREATE FOREIGN TABLE nested(a varchar OPTIONS (map 'topObj.midObj.botObj.a'),
                            b varchar OPTIONS (map 'topObj.midObj.botObj.b'),
                            c varchar OPTIONS (map 'topObj.midObj.botObj.c'))
       SERVER quasar
       OPTIONS (table 'nested');

SELECT city FROM zips LIMIT 3;

-- See the Quasar Query used
EXPLAIN SELECT city FROM zips;

-- See the Quasar and Mongo queries used
EXPLAIN (VERBOSE on) SELECT city FROM zips;
```

### Gotchyas

- If one of your fields uses a quasar-reserved word (such as `date`, you must quote the field using an attribute option: `OPTIONS (map 'comment."date"')`
- Postgres will downcase all field names, so if a field has a capital letter in it, you must use the map option: `OPTIONS (map "camelCaseSensitive")`
- This FDW will convert strings to other types, such as dates, times, timestamps, intervals, integers, and floats. However, if the underlying data is a string, you cannot push down type-specific operations such as WHERE clauses to Quasar by default. Therefore, you can enforce a no pushdown restriction in the column options. Use the `OPTIONS (nopushdown 'true')` option to force no pushdown of any clause containing the column.

## Development

### Testing

Setup:

1. Start mongodb
2. Import test data (`QUASAR_DIR=../quasar make import-test-data`)
3. Start Quasar (`QUASAR_DIR=../quasar make build-quasar start-quasar`)
4. Start Postgres

And to test:

```
make install installcheck
```

### Adding more pushdown features

If [quasar](https://github.com/quasar-analytics/quasar) adds operators, it would be good to update this FDW to support pushdown of that operator. This can be done in a few places:

- For operators like math (+ - / ^ ...) or regex (~~ LIKE ...)
  - search [quasar_fdw.c](src/quasar_fdw.c) for `/* OPERATORS */`
  - add the new operator to the big `if` statement
  - if the operator needs to be transformed to quasar syntax, do that below at `/* TRANSFORM BINARY OPERATOR */` or `/* TRANSFORM UNARY OPERATOR */`.
  - Adding a scalar array operator such as ANY should be done at `/* SCALAR ARRAY OPERATOR */`, although its a little bit more complicated.
- For functions like math (power) or string (capitalize)
  - search [quasar_fdw.c](src/quasar_fdw.c) for `/* FUNCTIONS */`
  - add the new function to the big `if` statement
  - if the function needs to be transformed, do it at `/* FUNCTION TRANSFORMS */`
- For more complicated things like ANY, CASE, NULLIF, etc, you'll have to find the correct case in the big switch statement in `getQuasarClause` and write the appropriate code to put the correct statement into `result`. Check out other cases to see what to do.

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
