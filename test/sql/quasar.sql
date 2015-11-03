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
CREATE FOREIGN TABLE zips(city varchar)
       SERVER quasar
       OPTIONS (table 'zips');
/* User Mapping */
CREATE USER MAPPING FOR current_user SERVER quasar;
/* Select */
SELECT city FROM zips LIMIT 3;
