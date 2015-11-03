# Quasar Foreign Data Wrapper for PostgreSQL

This FDW forwards SELECT statements to [Quasar](https://github.com/quasar-analytics/quasar).

## WIP Status

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
