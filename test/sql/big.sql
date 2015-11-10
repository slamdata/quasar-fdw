CREATE SERVER big_quasar FOREIGN DATA WRAPPER quasar_fdw OPTIONS (server 'http://localhost:8080', path '/local/quasar');
CREATE FOREIGN TABLE big_zips(city varchar, pop integer, state char(2))
       SERVER big_quasar OPTIONS (table 'zips');
/* Test a big query */
SELECT * FROM big_zips ORDER BY state,city;
