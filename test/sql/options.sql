/* CREATE SERVER options checks */
CREATE SERVER o_quasar0 FOREIGN DATA WRAPPER quasar_fdw ;
/* wrong options are illegal */
CREATE SERVER o_quasar1 FOREIGN DATA WRAPPER quasar_fdw OPTIONS (wrong 'foo');
/* repeated options is illegal */
CREATE SERVER o_quasar2 FOREIGN DATA WRAPPER quasar_fdw OPTIONS (server 'http://localhost:8080', path '/local/quasar', server 'http://localhost:8080');
/* timeout_ms 0 is illegal */
CREATE SERVER o_quasar3 FOREIGN DATA WRAPPER quasar_fdw OPTIONS (server 'http://localhost:8080', path '/local/quasar', timeout_ms '0');
/* CREATE TABLE options checks */
CREATE FOREIGN TABLE o_ft0(id integer) SERVER o_quasar;
/* wrong options are illegal */
CREATE FOREIGN TABLE o_ft1(id integer) SERVER o_quasar OPTIONS (wrong 'foo');
/* repeated options are illegal */
CREATE FOREIGN TABLE o_ft2(id integer) SERVER o_quasar OPTIONS (table 'bar', table 'baz');
/* Attribute Options */
/* wrong options are illegal */
CREATE FOREIGN TABLE o_bad_attr_opt(city varchar OPTIONS (none 'yoyo'))
       SERVER o_quasar OPTIONS (table 'zips');
