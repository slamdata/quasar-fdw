/* Explain */
/* Basic selection with limit */
EXPLAIN (COSTS off) SELECT * FROM zips LIMIT 3;
/* Select less fields than exist */
EXPLAIN (COSTS off) SELECT city FROM zips LIMIT 1;
/* Basic WHERE clause */
EXPLAIN (COSTS off) SELECT * FROM zips WHERE "state" = 'CO' LIMIT 2;
/* Nested selection */
EXPLAIN (COSTS off) SELECT * FROM nested LIMIT 1;
/* less fields than in relation, with one in a WHERE clause */
EXPLAIN (COSTS off) SELECT city FROM zips WHERE "state" = 'CO' LIMIT 1;
EXPLAIN (COSTS off) SELECT city,pop FROM zips WHERE pop % 2 = 1 LIMIT 3;
/* Test out array usage */
EXPLAIN (COSTS off) SELECT * FROM zipsloc LIMIT 2;
/* Test out json usage */
EXPLAIN (COSTS off) SELECT loc->0 AS loc0, locb->1 AS loc1, locb FROM zipsjson LIMIT 2;
/* Pushdown regex operators */
EXPLAIN (COSTS off) SELECT * FROM zips WHERE "state" LIKE 'A%' LIMIT 3;
EXPLAIN (COSTS off) SELECT * FROM zips WHERE "city" !~~ 'B%' LIMIT 3;
/* pushdown math operators */
EXPLAIN (COSTS off) SELECT * FROM zips WHERE pop > 1000 AND pop + pop <= 10000 LIMIT 3;
/* join zips and zipsjson */
EXPLAIN (COSTS off) SELECT zips.city AS city, pop, state, loc
                    FROM zips JOIN zipsjson ON zips.city = zipsjson.city
                    LIMIT 3;
/* query for a missing field */
EXPLAIN (COSTS off) SELECT missing, city FROM zips_missing LIMIT 3;
/* No pushdown of `nopushdown` columns */
EXPLAIN (COSTS off) SELECT * FROM commits WHERE ts = timestamp 'Thu Jan 29 15:52:37 2015';
/* Pushdown of concat */
EXPLAIN (COSTS off) SELECT * FROM smallzips WHERE length(concat(state, city)) > 4 AND state LIKE concat('M'::char, '%'::char) LIMIT 5;
/* pushdown of concat */
EXPLAIN (COSTS off) SELECT * FROM smallzips WHERE length(concat(state, city)) > 4 /* push down concat columns */
                                            AND state = concat('M'::char, 'A'::char) /* pushed down correctly */ LIMIT 5;
/* LIKE operator only supports constant right sides */
EXPLAIN (COSTS off) SELECT * FROM smallzips WHERE state LIKE concat('B'::char, '%'::char);
/* ORDER BY pushdown */
EXPLAIN (COSTS off) SELECT * FROM zips ORDER BY length(city), -pop, state;
/* If one ORDER BY column can't be pushed down, none are */
EXPLAIN (COSTS off) SELECT * FROM commits ORDER BY ts, sha;
