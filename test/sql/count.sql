/* Normal count */
select count(*) from zips;
/* Count with where */
select count(*) from zips where "state" = 'CO';
/* Count with group by */
select state, count(*) from zips group by state;
/* Count with field */
select count(city) from zips;
