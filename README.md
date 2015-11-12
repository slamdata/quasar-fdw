# Quasar Foreign Data Wrapper for PostgreSQL

This FDW forwards SELECT statements to [Quasar](https://github.com/quasar-analytics/quasar).

## WIP Status

11/11/2015:
- Tell postgres to not execute WHERE filters when we are pushing a WHERE filter down.
- Correctly serialize our private data between planning and executing. This was causing errors when memory contexts were switched for function execution.
- Add function execution support with parameter expansion.
- Add input data conversion check for integers that look like floats

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
    OPTIONS (server 'http://localhost:8080', path '/local/quasar');

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

## Testing

Setup:

1. Start mongodb
2. Import test data (`QUASAR_DIR=../quasar make import-test-data`)
3. Start Quasar (`QUASAR_DIR=../quasar make build-quasar start-quasar`)
4. Start Postgres

And to test:

```
make install installcheck
```


## License

????

## Attribution

This project was based on a skeleton FDW from [blackhole_fdw](https://bitbucket.org/adunstan/blackhole_fdw)

The download method for this fdw is based on the [s3_fdw](https://github.com/umitanuki/s3_fdw).

Quasar is a project created by [SlamData](http://slamdata.com).

This fdw uses [libcurl](http://curl.haxx.se/libcurl/).
