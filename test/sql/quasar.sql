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
       SERVER quasar OPTIONS (table 'zips');
CREATE FOREIGN TABLE zipsloc(loc numeric[2]) SERVER quasar OPTIONS (table 'zips');
CREATE FOREIGN TABLE zipsjson(loc json, locb jsonb OPTIONS (map 'loc'))
       SERVER quasar OPTIONS (table 'zips');
CREATE FOREIGN TABLE nested(a varchar OPTIONS (map 'topObj.midObj.botObj.a'),
                            b varchar OPTIONS (map 'topObj.midObj.botObj.b'),
                            c varchar OPTIONS (map 'topObj.midObj.botObj.c'))
       SERVER quasar
       OPTIONS (table 'nested');
/* Select */
/* Basic selection with limit */
SELECT * FROM zips LIMIT 3;
/* Select less fields than exist */
SELECT city FROM zips LIMIT 1;
/* Basic WHERE clause */
SELECT * FROM zips WHERE "state" = 'CO' LIMIT 2;
/* Nested selection */
SELECT * FROM nested LIMIT 1;
/* less fields than in relation, with one in a WHERE clause */
SELECT city FROM zips WHERE "state" = 'CO' LIMIT 1;
/* Test out array usage */
SELECT * FROM zipsloc LIMIT 2;
/* Test out json usage */
SELECT * FROM zipsjson LIMIT 2;
