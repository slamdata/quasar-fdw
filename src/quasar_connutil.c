/*-------------------------------------------------------------------------
 *
 * Quasar Foreign Data Wrapper for PostgreSQL
 *
 * Copyright (c) 2015 SlamData
 *
 * This software is released under the PostgreSQL Licence
 *
 * Author: Jon Eisen <jon@joneisen.works>
 *
 * IDENTIFICATION
 *            quasar_fdw/src/connutil.c
 *
 *-------------------------------------------------------------------------
 */
#include <time.h>
#include <sys/stat.h>

#include "postgres.h"
#include "quasar_fdw.h"

#include "miscadmin.h"
#include "lib/stringinfo.h"
#include "storage/fd.h"
#include "storage/ipc.h"

#define SIGNATURE_LENGTH 64

static char *httpdate(time_t *timer);
char *create_signature(void);

/**
 * Creates a file prefix (directory) given a signature
 */
char *create_tempprefix()
{
    char        filename[MAXPGPATH], path[MAXPGPATH];
    char *sig = create_signature();

    snprintf(filename, sizeof(filename), "%u.%s", MyProcPid, sig);
    mkdir("base/" PG_TEMP_FILES_DIR, S_IRWXU);
    snprintf(path, sizeof(path), "base/%s/%s", PG_TEMP_FILES_DIR, filename);

    pfree(sig);
    return pstrdup(path);
}

/**
 * Creates a pseudo-random signature for a given query
 */
char * create_signature() {
    char *date = httpdate(NULL);
    StringInfoData buf;

    initStringInfo(&buf);
    appendStringInfo(&buf, "%d__%s", rand(), date);

    pfree(date);
    return buf.data;
}

/*
 * Constructs GMT-style string
 */
static char * httpdate(time_t *timer)
{
    char   *datestring;
    time_t t;
    struct tm *gt;

    t = time(timer);
    gt = gmtime(&t);
    datestring = (char *) palloc0(256 * sizeof(char));
    strftime(datestring, 256 * sizeof(char), "%a,_%d_%b_%Y_%H-%M-%S_+0000", gt);
    return datestring;
}
