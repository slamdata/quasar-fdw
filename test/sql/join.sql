/* We are comparing JOIN explanations (and verifying outputs)
 * for tables with use_remote_estimate on and off */
/* Big joins are merge joins */
EXPLAIN SELECT * FROM zips_re z1, zips_re z2 WHERE z1.city = z2.city;
SELECT * FROM zips_re z1, zips_re z2 WHERE z1.city = z2.city LIMIT 10;
/* Hash join with no sort on large table joining smaller one */
EXPLAIN SELECT * FROM zips_re z1, zips_re z2 WHERE z1.pop > 60000 AND z1.city = z2.city;
SELECT * FROM zips_re z1, zips_re z2 WHERE z1.pop > 60000 AND z1.city = z2.city LIMIT 10;
/* Nested loop join when a tiny number of records */
EXPLAIN SELECT * FROM zips_re z1, zips_re z2 WHERE z1.pop > 60000 AND z1.state = 'MA' AND z1.city = z2.city;
SELECT * FROM zips_re z1, zips_re z2 WHERE z1.pop > 60000 AND z1.state = 'MA' AND z1.city = z2.city;
