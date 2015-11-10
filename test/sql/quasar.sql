CREATE SERVER quasar FOREIGN DATA WRAPPER quasar_fdw OPTIONS (server 'http://localhost:8080', path '/local/quasar');
CREATE FOREIGN TABLE zips(_id varchar, city varchar, pop integer, state char(2))
       SERVER quasar OPTIONS (table 'zips');
CREATE FOREIGN TABLE zipsloc(loc numeric[2]) SERVER quasar OPTIONS (table 'zips');
CREATE FOREIGN TABLE zipsjson(_id varchar, loc json, locb jsonb OPTIONS (map 'loc'))
       SERVER quasar OPTIONS (table 'zips');
CREATE FOREIGN TABLE nested(a varchar OPTIONS (map 'topObj.midObj.botObj.a'),
                            b varchar OPTIONS (map 'topObj.midObj.botObj.b'),
                            c varchar OPTIONS (map 'topObj.midObj.botObj.c'))
       SERVER quasar
       OPTIONS (table 'nested');
/* Select */
/* Basic selection with limit */
SELECT * FROM zips LIMIT 3;
/* Select less fields than exist */
SELECT city FROM zips LIMIT 1;
/* Basic WHERE clause */
SELECT * FROM zips WHERE "state" = 'CO' LIMIT 2;
/* Nested selection */
SELECT * FROM nested LIMIT 1;
/* less fields than in relation, with one in a WHERE clause */
SELECT city FROM zips WHERE "state" = 'CO' LIMIT 1;
SELECT city,pop FROM zips WHERE pop % 2 = 1 LIMIT 3;
/* Test out array usage */
SELECT * FROM zipsloc LIMIT 2;
/* Test out json usage */
SELECT loc->0 AS loc0, locb->1 AS loc1, locb FROM zipsjson LIMIT 2;
/* Pushdown regex operators */
SELECT * FROM zips WHERE "state" LIKE 'A%' LIMIT 3;
SELECT * FROM zips WHERE "city" !~~ 'B%' LIMIT 3;
/* pushdown math operators */
SELECT * FROM e_zips WHERE pop > 1000 AND pop + pop <= 10000 LIMIT 3;
/* join zips and zipsjson */
SELECT city,pop,state,loc FROM zips JOIN zipsjson ON zips._id = zipsjson._id LIMIT 3;
