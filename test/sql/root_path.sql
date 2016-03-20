/* move the path to the table instead of the server */
CREATE SERVER quasar_root FOREIGN DATA WRAPPER quasar_fdw
       OPTIONS (server 'http://localhost:8080'
               ,path '/');

CREATE FOREIGN TABLE zips_root(city varchar, pop integer, state char(2))
       SERVER quasar_root OPTIONS (table 'local/quasar/zips');

SELECT * FROM zips_root ORDER BY city,pop LIMIT 5;
