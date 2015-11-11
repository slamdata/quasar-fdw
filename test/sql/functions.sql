PREPARE statepop(char(2)) AS SELECT sum(pop) FROM zips WHERE state = $1;
EXECUTE statepop('CO');
EXECUTE statepop('RI');
DEALLOCATE statepop;
