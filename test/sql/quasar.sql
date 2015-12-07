/* Select */
/* Basic selection with limit */
SELECT * FROM smallzips ORDER BY city LIMIT 3;
/* Select less fields than exist */
SELECT city FROM smallzips ORDER BY city LIMIT 1;
/* Basic WHERE clause */
SELECT * FROM zips WHERE "state" = 'CO' ORDER BY city LIMIT 2;
/* Nested selection */
SELECT * FROM nested LIMIT 1;
/* less fields than in relation, with one in a WHERE clause */
SELECT city FROM zips WHERE "state" = 'CO' ORDER BY city LIMIT 1;
SELECT city,pop FROM smallzips WHERE pop % 2 = 1 ORDER BY city LIMIT 3;
/* Test out array usage */
SELECT * FROM zipsloc ORDER BY loc[1] LIMIT 2;
/* Test out json usage */
SELECT loc->0 AS loc0, locb->1 AS loc1, locb FROM zipsjson ORDER BY loc->0 LIMIT 2;
/* Pushdown regex operators */
SELECT * FROM zips WHERE "state" LIKE 'A%' ORDER BY city LIMIT 3;
SELECT * FROM smallzips WHERE "city" !~~ 'B%' ORDER BY city LIMIT 3;
/* pushdown math operators */
SELECT * FROM smallzips WHERE pop > 1000 AND pop + pop <= 10000 ORDER BY city LIMIT 3;
/* join zips and zipsjson */
SELECT zips.city AS city, pop, state, loc
       FROM smallzips zips JOIN zipsjson ON zips.city = zipsjson.city
       ORDER BY zips.city LIMIT 3;
/* query for a missing field */
SELECT missing, city FROM zips_missing ORDER BY city LIMIT 3;
/* query with a bad type */
SELECT loc FROM zips_bad LIMIT 1;
/* Make quasar_fdw convert a float to integer
 * Quasar converts pop to a float and we convert it back */
SELECT pop, loc0 FROM zips_convert WHERE loc0 < -70 ORDER BY pop LIMIT 3;
/* convert string timestamps to timestamp values */
SELECT * FROM commits ORDER BY ts LIMIT 5;
/* BUT we shouldn't push down comparisons on dates because the underlying data is string */
SELECT * FROM commits WHERE ts = timestamp 'Thu Jan 29 15:52:37 2015';
/* convert year as string to integers floats and dates */
SELECT yr, yearint, yearfloat FROM olympics WHERE "yr" = '1924' LIMIT 3;
/* pushdown of concat */
SELECT * FROM smallzips WHERE length(concat(state, city)) > 4 /* push down concat columns */
                        AND state = concat('M'::char, 'A'::char) /* pushed down correctly */ ORDER BY city LIMIT 5;
/* LIKE operator only supports constant right sides */
SELECT * FROM smallzips WHERE state LIKE concat('B'::char, '%'::char) ORDER BY city;
/* array expansion */
SELECT * FROM user_comments ORDER BY comment_text;
SELECT * FROM nested_expansion ORDER BY vals;
/* ORDER BY pushdown */
SELECT * FROM zips ORDER BY length(city), -pop, state LIMIT 10;
/* Date pushdown and ordering */
SELECT * FROM commits_timestamps WHERE ts < TIMESTAMP '2015-01-20T00:00:00Z' ORDER BY ts LIMIT 2;
SELECT * FROM commits_timestamps WHERE ts < DATE '2015-01-20' ORDER BY ts LIMIT 2;
SELECT * FROM commits_timestamps WHERE tstz < TIMESTAMPTZ '2015-01-15 19:43:04 PST' ORDER BY ts;
SELECT * FROM commits_timestamps ORDER BY ts DESC LIMIT 2;
/* Bad types should emit error contexts appropriately */
SELECT city FROM zips_badtype ORDER BY city;
SELECT state FROM zips_badtype ORDER BY state;
SELECT pop FROM zips_badtype ORDER BY pop;
/* Aggregations */
SELECT count(*) FROM smallzips;
SELECT count(*) FROM zips WHERE state IN ('CA','OR','WA');
/* Array subscripts push down correctly from 1-based to 0-based */
SELECT loc FROM zipsloc WHERE loc[1] < 0 ORDER BY loc[1] LIMIT 3;
SELECT loc FROM zipsloc WHERE loc[1+1] > 0 ORDER BY loc[2] LIMIT 3;
/* Scalar Array ops */
SELECT * FROM smallzips WHERE state IN ('MA', 'CA') ORDER BY city,pop LIMIT 3;
SELECT * FROM smallzips WHERE state IN ('MA') ORDER BY city,pop LIMIT 3;
