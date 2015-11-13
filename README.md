# Quasar Foreign Data Wrapper for PostgreSQL

This FDW forwards SELECT statements to [Quasar](https://github.com/quasar-analytics/quasar).


The main advantage of using this FDW over alternatives is that it takes full advantage of the Quasar query engine by "pushing down" as many clauses from the PostgreSQL query to Quasar as possible. This includes WHERE, ORDER BY, and JOIN clauses.

## WIP Status

11/13/2015:
- Even though [SQL/MED](https://wiki.postgresql.org/wiki/SQL/MED) claims ORDER BY clauses can't be pushed down, they can! I just got that to work.
- After much reading through the `postgres_fdw`, I have figured out how to optimize JOINs. Essentially, we cannot ever fully push a JOIN down to Quasar. But we can optimize the queries to minimally scan the tables. Also to do this, I have to rewrite the entire query creation part of the system. I'm about 70% the way through that.
- Next Steps:
    - Finish rewrite and optimize joins (with tests)
    - Add remote query estimation ability (good for optimizing join scans between tables with different sizes)
    - Add ANALYZE / EXPLAIN VERBOSE syntax
    - Create packaging script
    - Test on various platforms

11/11/2015:
- Tell postgres to not execute WHERE filters when we are pushing a WHERE filter down.
- Correctly serialize our private data between planning and executing. This was causing errors when memory contexts were switched for function execution.
- Add function execution support with parameter expansion.
- Add input data conversion check for integers that look like floats
- Talked with @johndegoes about things to ensure that work as well as next steps on packaging and QA
- Ensured string->integer, string->date, float(.0)->integer conversions work.
- Figured out a weird thing, if a date is string in underlying data and the schema calls it a date, we could try to pushdown a WHERE clause on the date, but quasar wouldn't be able to handle it. As such I added a new option on attributes called `nopushdown`, which will abandon pushing down any WHERE clause to quasar containing that column.
- Tested some more functions like string concat
- Test array expansion in queries
- Switch to curl_multi interface to stream data without forked processes and without buffering all request data before processing it.
- Add timeout_ms option
- Tested with [yajl_reset branch](https://github.com/lloyd/yajl/tree/yajl_reset), which was written by the creator of yajl, and is equivalent to my PR. It should be merged soon, but thats the required branch for the moment.

11/10/2015:
- I patched the json parser to have a reset feature so I didn't have to allocate every iteration. Interestingly, the parser lazy-allocates its lexer, which causes issues on the second iteration because each iteration is in a short-lived memory context. I had to force an allocation with `yajl_parse(handle, NULL, 0)` in the `BeginForeignScan` function in order to fix it.
- Various fixes this morning:
  - add support for rescans (needed for non-pushdown joins)
  - add support for missing fields in quasar, set to NULL in returned tuple
  - remove non-unix compatible stuff (FIFO files)
  - split out a bunch of new test cases into readable chunks
  - fix the input function calling to support array types

11/9/2015:
- I implemented a parser in `quasar_fdw`. It currently handles all base types (I haven't tested dates/times/intervals, but they should work), and arbitrary json. It does not support array types at the moment, but json can be a stand-in for now.
- The json parser I used is missing a "pause" feature to only parse a single row at a time. I believe it will be simple to patch, so I'll make a PR for that. For now, I'm doing extra allocation every iteration.
- I also got EXPLAIN working so I can test pushdown.
- Next Steps:
    - Add Date/time/interval test cases with data parsing and WHERE pushdown
    - Implement JOIN pushdown
    - Add param support
    - Add array datatype support
    - Replace forked `easy_curl` with `curl_multi`
    - Do yajl pause feature

11/4/2015:
- I found out a way to tell the Copy command that we're only grabbing certain fields ([here](https://github.com/postgres/postgres/blob/master/src/backend/commands/copy.c#L2603)). This solves one problem I had last week without rewriting the parser. If I can leverage `AS` syntax to overcome parsing arrays, I might be able to not have to rewrite the parser at all.
- After talking with Greg, I think the best way to get the returned data ingested in correct formatting is to _not_ write a parser in C, but instead create a new output format for Quasar based on Postgres' COPY command. (See this [JIRA ticket](https://slamdata.atlassian.net/browse/SD-1096)). Moss seemed to agree. Not sure when work on this will start. After I get the WHERE and JOIN pushdown done, I'll circle around and implement it if no one else already has.
- Finally, I was able to implement WHERE pushdown. A lot of thanks to the oracle_fdw guys there. I haven't added a ton of test cases, but basic usage is there.
- Also found out we won't be able to push down LIMIT, ORDER BY, or GROUP BY as those aren't exposed to FDWs.
- Next Steps:
    - Test `WHERE` clauses
    - Add `EXPLAIN` support and test cases to test pushdown
    - Add `JOIN` support
    - Add `text/csv;mode=postgres` to [Quasar](https://slamdata.atlassian.net/browse/SD-1096)

11/3/2015:
- Did a ton of reading on the [oracle_fdw](https://github.com/laurenz/oracle_fdw). The oracle fdw pushes down a lot of WHERE clauses and only grabs the fields needed in those clauses.
- I was able to get SELECT working by only grabbing the necessary columns, as desired in the `RelPlanInfo` (see [here](https://github.com/yanatan16/quasar_fdw/blob/8fa17d1cbb7e5d863885d060fdb154fdbe767471/src/quasar_fdw.c#L713))
- However, there doesn't seem to be a way to tell postgres that "I'm only grabbing the columns you asked for." It seems to think you always are returning all the columns. In fact, oracle_fdw gives a Tuple with those dropped columns set to NULL [here](https://github.com/laurenz/oracle_fdw/blob/master/oracle_fdw.c#L4709).
    - Implication: Instead of relying on the Copy protocol to read the CSV back from Quasar, I'll have to write my own parsing system and translate that to Tuples.
    - Plan: Keep the forked curl piping to a FIFO file. Instead of passing to the Copy protocol to read, I'll read it as json into `libjson`. Once I've read a record, I parse that into a Tuple, similar to oracle_fdw.
    - Benefits: This system can handle not requesting dropped columns, as well as the array types I'm pretty sure don't work with Copy/csv already.
- I added the column name mapping feature. See the example below. Don't support arrays yet so we'll have to wait to truly test it.
- Next Steps:
    - Support `WHERE` clauses.
    - Rewrite api response parsing (see above)
    - Support `LIMIT` and `ORDER BY` (AFAICT oracle_fdw doesn't support these)
    - Support `EXPLAIN` to add tests about what the query to Quasar should be

11/2/2015:
- Threw out `array.c`. As soon as I found out about postgres' linked list implementation, I started using that.
- But, as far as I can tell, theres no way to build an executable against that code because its not wrapped up in a lib. Thus I couldn't run any tests over that code in an automatable way.
- Got a `SELECT` statement working for hardcoded query. I was able to use the method use in [s3_fdw](https://github.com/umitanuki/s3_fdw), which does a curl download in a forked process and fifo's the result over to postgres.
- Because of using the `s3_fdw` method, I threw out `quasar_api.c`.
- Next up, I need to dive deep into understand the postgres exectutor. First, I need to understand how a SELECT for a single field in a table of multiple fields is represented in the Execution Plan.

11/1/2015 - Continued work on `quasar_api.c`, implemented my own `array.c`. Got api queries working in `check` tests.

10/31/2015 - Got a blackhole_fdw build system working. Started work on `quasar_api.c`

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
