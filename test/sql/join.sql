/* We are comparing JOIN explanations (and verifying outputs)
 * for tables with use_remote_estimate on and off */
/* Big joins are merge joins */
EXPLAIN (COSTS off) SELECT * FROM zips_re z1, zips_re z2 WHERE z1.city = z2.city;
SELECT * FROM zips_re z1, zips_re z2 WHERE z1.city = z2.city ORDER BY z1.city, z1.pop LIMIT 10;
/* Hash join with no sort on large table joining smaller one */
EXPLAIN (COSTS off) SELECT * FROM zips_re z1, zips_re z2 WHERE z1.pop > 60000 AND z1.city = z2.city;
SELECT * FROM zips_re z1, zips_re z2 WHERE z1.pop > 60000 AND z1.city = z2.city ORDER BY z1.city, z1.pop LIMIT 10;
/* Nested loop join when a tiny number of records */
EXPLAIN (COSTS off) SELECT * FROM zips_re z1, zips_re z2 WHERE z1.pop > 60000 AND z1.state = 'MA' AND z1.city = z2.city;
SELECT * FROM zips_re z1, zips_re z2 WHERE z1.pop > 60000 AND z1.state = 'MA' AND z1.city = z2.city ORDER BY z1.pop;
/* Cross Join */
EXPLAIN (COSTS off) SELECT * FROM smallzips CROSS JOIN nested;
SELECT * FROM smallzips CROSS JOIN nested ORDER BY city LIMIT 2;
/* Outer Join */
EXPLAIN (COSTS off) SELECT * FROM smallzips z1 LEFT OUTER JOIN zips_missing z2 ON z1.city = z2.missing;
SELECT * FROM smallzips z1 LEFT OUTER JOIN zips_missing z2 ON z1.city = z2.missing ORDER BY z1.city LIMIT 2;
EXPLAIN (COSTS off) SELECT * FROM smallzips z1 RIGHT OUTER JOIN zips_missing z2 ON z1.city = z2.missing;
SELECT * FROM smallzips z1 RIGHT OUTER JOIN zips_missing z2 ON z1.city = z2.missing ORDER BY z2.city LIMIT 2;
/* zips_re state field has a join_rowcount_estimate of 500 so it will use Hash join on some small joins */
EXPLAIN (COSTS off) SELECT * FROM zips_re z1, zips_re z2 WHERE z1.state = z2.state AND z1.city IN ('BARRE', 'AGAWAM');
