/* Explain */
/* Basic selection with limit */
EXPLAIN SELECT * FROM zips LIMIT 3;
/* Select less fields than exist */
EXPLAIN SELECT city FROM zips LIMIT 1;
/* Basic WHERE clause */
EXPLAIN SELECT * FROM zips WHERE "state" = 'CO' LIMIT 2;
/* Nested selection */
EXPLAIN SELECT * FROM nested LIMIT 1;
/* less fields than in relation, with one in a WHERE clause */
EXPLAIN SELECT city FROM zips WHERE "state" = 'CO' LIMIT 1;
EXPLAIN SELECT city,pop FROM zips WHERE pop % 2 = 1 LIMIT 3;
/* Test out array usage */
EXPLAIN SELECT * FROM zipsloc LIMIT 2;
/* Test out json usage */
EXPLAIN SELECT loc->0 AS loc0, locb->1 AS loc1, locb FROM zipsjson LIMIT 2;
/* Pushdown regex operators */
EXPLAIN SELECT * FROM zips WHERE "state" LIKE 'A%' LIMIT 3;
EXPLAIN SELECT * FROM zips WHERE "city" !~~ 'B%' LIMIT 3;
/* pushdown math operators */
EXPLAIN SELECT * FROM zips WHERE pop > 1000 AND pop + pop <= 10000 LIMIT 3;
/* join zips and zipsjson */
EXPLAIN SELECT zips.city AS city, pop, state, loc
               FROM zips JOIN zipsjson ON zips.city = zipsjson.city
               LIMIT 3;
