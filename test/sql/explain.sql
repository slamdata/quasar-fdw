CREATE SERVER e_quasar FOREIGN DATA WRAPPER quasar_fdw OPTIONS (server 'http://localhost:8080', path '/local/quasar');
CREATE FOREIGN TABLE e_zips(city varchar, pop integer, state char(2))
       SERVER e_quasar OPTIONS (table 'zips');
CREATE FOREIGN TABLE e_zipsloc(loc numeric[2])
       SERVER e_quasar OPTIONS (table 'zips');
CREATE FOREIGN TABLE e_zipsjson(loc json, locb jsonb OPTIONS (map 'loc'))
       SERVER e_quasar OPTIONS (table 'zips');
CREATE FOREIGN TABLE e_nested(a varchar OPTIONS (map 'topObj.midObj.botObj.a'),
                              b varchar OPTIONS (map 'topObj.midObj.botObj.b'),
                              c varchar OPTIONS (map 'topObj.midObj.botObj.c'))
       SERVER e_quasar OPTIONS (table 'nested');
/* Select */
/* Basic selection with limit */
EXPLAIN SELECT * FROM e_zips LIMIT 3;
/* Select less fields than exist */
EXPLAIN SELECT city FROM e_zips LIMIT 1;
/* Basic WHERE clause */
EXPLAIN SELECT * FROM e_zips WHERE "state" = 'CO' LIMIT 2;
/* Nested selection */
EXPLAIN SELECT * FROM e_nested LIMIT 1;
/* less fields than in relation, with one in a WHERE clause */
EXPLAIN SELECT city FROM e_zips WHERE "state" = 'CO' LIMIT 1;
EXPLAIN SELECT city,pop FROM e_zips WHERE pop % 2 = 1 LIMIT 3;
/* Test out array usage */
EXPLAIN SELECT * FROM e_zipsloc LIMIT 2;
/* Test out json usage */
EXPLAIN SELECT loc->0 AS loc0, locb->1 AS loc1, locb FROM e_zipsjson LIMIT 2;
/* Pushdown regex operators */
EXPLAIN SELECT * FROM e_zips WHERE "state" LIKE 'A%' LIMIT 3;
EXPLAIN SELECT * FROM e_zips WHERE "city" !~~ 'B%' LIMIT 3;
/* pushdown math operators */
EXPLAIN SELECT * FROM e_zips WHERE pop > 1000 AND pop + pop <= 10000 LIMIT 3;
