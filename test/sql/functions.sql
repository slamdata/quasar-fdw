PREPARE statepop(char(2)) AS SELECT sum(pop) FROM zips WHERE state = $1;
EXECUTE statepop('CO');
EXECUTE statepop('RI');
EXPLAIN (COSTS off) EXECUTE statepop('MA');
DEALLOCATE statepop;
