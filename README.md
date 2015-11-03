# Quasar Foreign Data Wrapper for PostgreSQL

This FDW forwards SELECT statements to [Quasar](https://github.com/quasar-analytics/quasar).

## WIP Status

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
