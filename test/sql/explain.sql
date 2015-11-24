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
EXPLAIN (COSTS off) SELECT * FROM smallzips
        /* push down concat columns */
        WHERE length(concat(state, city)) > 4
        /* pushed down correctly */
        AND state = concat('M'::char, 'A'::char)
        /* push down concat operator */
        AND 'B' || city LIKE 'B%' LIMIT 5;
/* LIKE operator only supports constant right sides */
EXPLAIN (COSTS off) SELECT * FROM smallzips WHERE state LIKE concat('B'::char, '%'::char);
/* ORDER BY pushdown */
EXPLAIN (COSTS off) SELECT * FROM zips ORDER BY length(city), pop DESC, state;
/* Can't pushdown NULLS FIRST */
EXPLAIN (COSTS off) SELECT * FROM zips ORDER BY state NULLS FIRST;
/* If one ORDER BY column can't be pushed down, none are */
EXPLAIN (COSTS off) SELECT * FROM commits ORDER BY ts, sha;
/* VERBOSE on */
EXPLAIN (COSTS off, VERBOSE on) SELECT * FROM zips WHERE state = 'CO' ORDER BY pop DESC;
/* Timestamps and dates pushdown */
EXPLAIN (COSTS off) SELECT * FROM commits_timestamps WHERE ts < TIMESTAMP '2015-01-20T00:00:00Z' LIMIT 2;
EXPLAIN (COSTS off) SELECT * FROM commits_timestamps WHERE ts < DATE '2015-01-20' LIMIT 2;
EXPLAIN (COSTS off) SELECT * FROM commits_timestamps WHERE tstz < TIMESTAMPTZ '2015-01-15 19:43:04 PST';
EXPLAIN (COSTS off) SELECT * FROM commits_timestamps ORDER BY ts DESC LIMIT 2;
EXPLAIN (COSTS off) SELECT * FROM test_times WHERE t < TIME '11:04:23.551';
/* Intervals less than 1 month are pushed down to quasar */
EXPLAIN (COSTS off) SELECT * FROM test_intervals WHERE i < INTERVAL '7 days 4 hours 5 minutes';
/* Intervals > 1 month can't be pushed down because quasar doesn't handle them */
EXPLAIN (COSTS off) SELECT * FROM test_intervals WHERE i > INTERVAL '1 year';
/* Aggregations */
EXPLAIN (COSTS off) SELECT count(*) FROM smallzips;
EXPLAIN (COSTS off) SELECT count(*) FROM zips WHERE state IN ('CA','OR','WA');
/* Array subscripts push down correctly from 1-based to 0-based */
EXPLAIN (COSTS off) SELECT loc FROM zipsloc WHERE loc[1] < 0;
EXPLAIN (COSTS off) SELECT loc FROM zipsloc WHERE loc[1+1] > 0;
