CREATE SERVER quasar FOREIGN DATA WRAPPER quasar_fdw OPTIONS (server 'http://localhost:8080', path '/local/quasar');
CREATE FOREIGN TABLE zips(city varchar, pop integer, state char(2))
SERVER quasar OPTIONS (table 'zips');
CREATE FOREIGN TABLE zipsloc(loc numeric[2]) SERVER quasar OPTIONS (table 'zips');
CREATE FOREIGN TABLE zipsjson(city varchar, loc json, locb jsonb OPTIONS (map 'loc'))
SERVER quasar OPTIONS (table 'zips');
CREATE FOREIGN TABLE nested(a varchar OPTIONS (map 'topObj.midObj.botObj.a'),
b varchar OPTIONS (map 'topObj.midObj.botObj.b'),
c varchar OPTIONS (map 'topObj.midObj.botObj.c'))
SERVER quasar
OPTIONS (table 'nested');
