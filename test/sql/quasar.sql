/* CREATE SERVER options checks */
CREATE SERVER quasar0 FOREIGN DATA WRAPPER quasar_fdw ;
CREATE SERVER quasar1 FOREIGN DATA WRAPPER quasar_fdw OPTIONS (wrong 'foo');
CREATE SERVER quasar2 FOREIGN DATA WRAPPER quasar_fdw OPTIONS (server 'http://localhost:8080', path '/local/quasar', server 'http://localhost:8080');
CREATE SERVER quasar FOREIGN DATA WRAPPER quasar_fdw OPTIONS (server 'http://localhost:8080', path '/local/quasar');
/* CREATE TABLE options checks */
CREATE FOREIGN TABLE ft0(id integer) SERVER quasar;
CREATE FOREIGN TABLE ft1(id integer) SERVER quasar OPTIONS (wrong 'foo');
CREATE FOREIGN TABLE ft2(id integer) SERVER quasar OPTIONS (table 'bar');
CREATE FOREIGN TABLE ft3(id integer) SERVER quasar OPTIONS (table 'bar', table 'baz');
CREATE FOREIGN TABLE zips(city varchar, pop integer, state char(2))
       SERVER quasar
       OPTIONS (table 'zips');
/* Select */
/* Basic selection with limit */
SELECT * FROM zips LIMIT 3;
/* Select less fields than exist */
SELECT city FROM zips LIMIT 1;
/* Basic WHERE clause */
SELECT * FROM zips WHERE "state" = 'CO' LIMIT 2;
