/* Select */
/* Basic selection with limit */
SELECT * FROM smallzips LIMIT 3;
/* Select less fields than exist */
SELECT city FROM smallzips LIMIT 1;
/* Basic WHERE clause */
SELECT * FROM zips WHERE "state" = 'CO' LIMIT 2;
/* Nested selection */
SELECT * FROM nested LIMIT 1;
/* less fields than in relation, with one in a WHERE clause */
SELECT city FROM zips WHERE "state" = 'CO' LIMIT 1;
SELECT city,pop FROM smallzips WHERE pop % 2 = 1 LIMIT 3;
/* Test out array usage */
SELECT * FROM zipsloc LIMIT 2;
/* Test out json usage */
SELECT loc->0 AS loc0, locb->1 AS loc1, locb FROM zipsjson LIMIT 2;
/* Pushdown regex operators */
SELECT * FROM zips WHERE "state" LIKE 'A%' LIMIT 3;
SELECT * FROM smallzips WHERE "city" !~~ 'B%' LIMIT 3;
/* pushdown math operators */
SELECT * FROM smallzips WHERE pop > 1000 AND pop + pop <= 10000 LIMIT 3;
/* join zips and zipsjson */
SELECT zips.city AS city, pop, state, loc
       FROM smallzips zips JOIN zipsjson ON zips.city = zipsjson.city
       ORDER BY zips.city LIMIT 3;
/* query for a missing field */
SELECT missing, city FROM zips_missing LIMIT 3;
/* query with a bad type */
SELECT loc FROM zips_bad LIMIT 1;
/* Make quasar_fdw convert a float to integer
 * Quasar converts pop to a float and we convert it back */
SELECT pop, loc0 FROM zips_convert WHERE loc0 < -70 LIMIT 3;
/* convert string timestamps to timestamp values */
SELECT * FROM commits LIMIT 5;
