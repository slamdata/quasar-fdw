/* We are comparing JOIN explanations (and verifying outputs)
 * for tables with use_remote_estimate on and off */
/* Merge join because theres no remote estimation on */
EXPLAIN SELECT z1.city, z2.state FROM smallzips z1, zips z2 WHERE z1.city = z2.city;
SELECT z1.city, z2.state FROM smallzips z1, zips z2 WHERE z1.city = z2.city LIMIT 10;
/* Hash join with no sort after remote estimation is on */
EXPLAIN SELECT z1.city, z2.state FROM smallzips_re z1, zips_re z2 WHERE z1.city = z2.city;
SELECT z1.city, z2.state FROM smallzips_re z1, zips_re z2 WHERE z1.city = z2.city LIMIT 10;
/* Nested loop because we only want a small number of records */
EXPLAIN SELECT z1.city, z2.state FROM smallzips_re z1, zips_re z2 WHERE z1.city = z2.city AND z1.city = 'BARRE';
SELECT z1.city, z2.state FROM smallzips_re z1, zips_re z2 WHERE z1.city = z2.city AND z1.city = 'BARRE';
