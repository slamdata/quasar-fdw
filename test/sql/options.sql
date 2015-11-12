/* CREATE SERVER options checks */
CREATE SERVER o_quasar0 FOREIGN DATA WRAPPER quasar_fdw ;
CREATE SERVER o_quasar1 FOREIGN DATA WRAPPER quasar_fdw OPTIONS (wrong 'foo');
CREATE SERVER o_quasar2 FOREIGN DATA WRAPPER quasar_fdw OPTIONS (server 'http://localhost:8080', path '/local/quasar', server 'http://localhost:8080');
CREATE SERVER o_quasar FOREIGN DATA WRAPPER quasar_fdw OPTIONS (server 'http://localhost:8080', path '/local/quasar');
/* CREATE TABLE options checks */
CREATE FOREIGN TABLE o_ft0(id integer) SERVER o_quasar;
CREATE FOREIGN TABLE o_ft1(id integer) SERVER o_quasar OPTIONS (wrong 'foo');
CREATE FOREIGN TABLE o_ft2(id integer) SERVER o_quasar OPTIONS (table 'bar');
CREATE FOREIGN TABLE o_ft3(id integer) SERVER o_quasar OPTIONS (table 'bar', table 'baz');
CREATE FOREIGN TABLE o_zips(city varchar, pop integer, state char(2))
       SERVER o_quasar OPTIONS (table 'zips');
/* Attribute Options */
CREATE FOREIGN TABLE o_bad_attr_opt(city varchar OPTIONS (none 'yoyo'))
       SERVER o_quasar OPTIONS (table 'zips');
CREATE FOREIGN TABLE o_nested(a varchar OPTIONS (map 'topObj.midObj.botObj.a',
                                                 nopushdown 'true'),
                              b varchar OPTIONS (map 'topObj.midObj.botObj.b'),
                              c varchar OPTIONS (map 'topObj.midObj.botObj.c'))
       SERVER o_quasar OPTIONS (table 'nested');
