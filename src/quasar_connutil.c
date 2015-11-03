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
static int b64_encode(const uint8 *src, unsigned len, uint8 *dst);

/**
 * Creates a file prefix (directory) given a signature
 */
char *create_tempprefix(char *sig)
{
    char        filename[MAXPGPATH], path[MAXPGPATH], *s;

    snprintf(filename, sizeof(filename), "%u.%s", MyProcPid, sig);
    s = &filename[0];
    while(*s)
    {
        if (*s == '/')
            *s = '%';
        s++;
    }
    mkdir("base/" PG_TEMP_FILES_DIR, S_IRWXU);
    snprintf(path, sizeof(path), "base/%s/%s", PG_TEMP_FILES_DIR, filename);

    return pstrdup(path);
}

/**
 * Creates a signature for a given query
 */
char * quasar_signature(char *query, char *server, char *path) {
    char *sig = palloc0(SIGNATURE_LENGTH * sizeof(char));
    char *date = httpdate(NULL);
    StringInfoData buf;
    initStringInfo(&buf);
    appendStringInfo(&buf, "%d%s%s%s%s", rand(), date, query, server, path);

    b64_encode((uint8*)buf.data, SIGNATURE_LENGTH, (uint8*) sig);
    pfree(buf.data);
    pfree(date);

    return sig;
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
    strftime(datestring, 256 * sizeof(char), "%a, %d %b %Y %H:%M:%S +0000", gt);
    return datestring;
}


static int
b64_encode(const uint8 *src, unsigned len, uint8 *dst)
{
    static const unsigned char _base64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    uint8      *p,
        *lend = dst + 76;
    const uint8 *s,
        *end = src + len;
    int         pos = 2;
    unsigned long buf = 0;

    s = src;
    p = dst;

    while (s < end)
    {
        buf |= *s << (pos << 3);
        pos--;
        s++;

        /*
         * write it out
         */
        if (pos < 0)
        {
            *p++ = _base64[(buf >> 18) & 0x3f];
            *p++ = _base64[(buf >> 12) & 0x3f];
            *p++ = _base64[(buf >> 6) & 0x3f];
            *p++ = _base64[buf & 0x3f];

            pos = 2;
            buf = 0;
        }
        if (p >= lend)
        {
            *p++ = '\n';
            lend = p + 76;
        }
    }
    if (pos != 2)
    {
        *p++ = _base64[(buf >> 18) & 0x3f];
        *p++ = _base64[(buf >> 12) & 0x3f];
        *p++ = (pos == 0) ? _base64[(buf >> 6) & 0x3f] : '=';
        *p++ = '=';
    }

    return p - dst;
}
