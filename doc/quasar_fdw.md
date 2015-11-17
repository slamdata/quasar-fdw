# Quasar Foreign Data Wrapper for PostgreSQL

This FDW forwards SELECT statements to [Quasar](https://github.com/quasar-analytics/quasar).

The main advantage of using this FDW over alternatives is that it takes full advantage of the Quasar query engine by "pushing down" as many clauses from the PostgreSQL query to Quasar as possible. This includes WHERE, ORDER BY, and JOIN clauses.

## Options

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
