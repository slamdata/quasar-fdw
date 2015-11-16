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
 *            quasar_fdw/src/quasar_parse.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "quasar_fdw.h"

#include "yajl/yajl_parse.h"

#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "common/fe_memutils.h"
#include "utils/syscache.h"


#define YAJL_OK 1
#define YAJL_CANCEL 0

#define NO_COLUMN -1

#define TOP_LEVEL 0
#define COLUMN_LEVEL 1

typedef struct parser {
    Datum *values;
    bool *nulls;
    AttInMetadata *attinmeta;

    size_t cur_col;
    bool record_complete;
    int level;
    StringInfoData json;
    StringInfoData array;
    int warned;
} parser;

/* Utilities */
static Form_pg_attribute get_column(parser *p);
static bool is_array_type(parser *p);
static bool is_json_type(parser *p);
static void jsonAppendCommaIf(parser *p);
static void arrayAppendCommaIf(parser *p);
static void store_datum(parser *p, char *string, const char *fmt);
static void store_null(parser *p);
bool warncheck(parser *p);
char *checkConversions(parser *p, char *value);

/* Alloc callbacks */
void *yajl_palloc(void *ctx, size_t sz);
void *yajl_repalloc(void *ctx, void *ptr, size_t sz);
void yajl_pfree(void *ctx, void *ptr);

/* Parse callbacks */
static int cb_null(void * ctx);
static int cb_boolean(void * ctx, int boolean);
static int cb_string(void * ctx, const unsigned char * value, size_t len);
static int cb_number(void * ctx, const char * value, size_t len);
static int cb_map_key(void * ctx, const unsigned char * stringVal, size_t stringLen);
static int cb_start_map(void * ctx);
static int cb_end_map(void * ctx);
static int cb_start_array(void * ctx);
static int cb_end_array(void * ctx);

bool warncheck(parser *p) {
    if (p->warned == 0) {
        p->warned = 1;
        return true;
    }
    return false;
}

static Form_pg_attribute get_column(parser *p) {
    if (p->cur_col != NO_COLUMN && p->cur_col < p->attinmeta->tupdesc->natts) {
        return p->attinmeta->tupdesc->attrs[p->cur_col];
    } else {
        elog(ERROR, "quasar_fdw internal: Got a value when no column specified!");
    }
}

static bool is_array_type(parser *p) {
    return get_column(p)->attndims > 0;
}

static bool is_json_type(parser *p) {
    switch(get_column(p)->atttypid) {
    case JSONOID:
    case JSONBOID:
        return true;
    default:
        return false;
    }
}

static void arrayAppendCommaIf(parser *p) {
    StringInfo s = &p->array;
    if (s->len <= 0) return;
    switch(s->data[s->len - 1]) {
    case '{':
    case ',':
        return;
    default:
        appendStringInfoChar(s, ',');
    }
}

static void jsonAppendCommaIf(parser *p) {
    StringInfo s = &p->json;
    if (s->len <= 0) return;
    switch(s->data[s->len - 1]) {
    case '{':
    case '[':
    case ':':
    case ',':
        return;
    default:
        appendStringInfoChar(s, ',');
    }
}

char *checkConversions(parser *p, char *value) {
    Form_pg_attribute col = get_column(p);

    if (col->attndims > 0) return value;
    size_t len = strlen(value);

    switch (col->atttypid) {
    case INT2OID:
    case INT4OID:
        /* Sometimes we get floats with .0 from quasar for ints */
        if (len > 2 && strcmp(value + len - 2, ".0") == 0)
            value[len - 2] = '\0';
        return value;
    default:
        return value;
    }
}

static void store_datum(parser *p, char *string, const char *fmt) {
    int i = p->cur_col;

    elog(DEBUG4, "store_datum: level %d, %s", p->level, string);

    if (p->level == COLUMN_LEVEL &&
        !get_column(p)->attisdropped) {
        string = checkConversions(p, string);
        p->values[i] = InputFunctionCall(&p->attinmeta->attinfuncs[i],
                                         string,
                                         p->attinmeta->attioparams[i],
                                         p->attinmeta->atttypmods[i]);
        p->nulls[i] = false;
    } else if (p->level > COLUMN_LEVEL && is_json_type(p)) {
        jsonAppendCommaIf(p);
        appendStringInfo(&p->json, fmt, string);
    } else if (p->level > COLUMN_LEVEL && is_array_type(p)) {
        arrayAppendCommaIf(p);
        appendStringInfo(&p->array, fmt, string);
    }
}


static void store_null(parser *p) {
    /* Assert the column is valid */

    if (p->level > COLUMN_LEVEL && is_json_type(p)) {
        jsonAppendCommaIf(p);
        appendStringInfo(&p->json, "null");
    } else if (p->level > COLUMN_LEVEL && is_array_type(p))  {
        arrayAppendCommaIf(p);
        appendStringInfo(&p->array, "NULL");
    } else if (p->level == COLUMN_LEVEL) {
        /* We initialize each field to null already... */
        return;
    } else {
        elog(ERROR, "quasar_fdw internal: storing null when level is above columns");
    }
}

/* Yajl callbacks */
static int cb_null(void * ctx) {
    elog(DEBUG4, "entering function %s", __func__);
    store_null((parser*) ctx);
    return YAJL_OK;
}

static int cb_boolean(void * ctx, int boolean) {
    elog(DEBUG4, "entering function %s", __func__);
    store_datum((parser*) ctx, boolean ? "true" : "false", "%s");
    return YAJL_OK;
}


static int cb_string(void * ctx,
                    const unsigned char * value,
                    size_t len) {
    elog(DEBUG4, "entering function %s", __func__);
    char * s = pnstrdup((const char *)value, len);
    store_datum((parser*) ctx, s, "\"%s\"");
    pfree(s);
    return YAJL_OK;
}

static int cb_number(void * ctx,
                     const char * value,
                     size_t len) {
    elog(DEBUG4, "entering function %s", __func__);
    char * s = pnstrdup(value, len);
    store_datum((parser*) ctx, s, "%s");
    pfree(s);
    return YAJL_OK;
}

static int cb_map_key(void * ctx,
                      const unsigned char * stringVal,
                      size_t stringLen) {
    elog(DEBUG4, "entering function %s", __func__);

    parser *p = (parser*) ctx;
    size_t i;
    char * s = pnstrdup((const char*) stringVal, stringLen);

    if (p->level == COLUMN_LEVEL) {
        /* Find the column */
        p->cur_col = NO_COLUMN;
        for (i = 0; i < p->attinmeta->tupdesc->natts; ++i) {
            if (strcmp(s, NameStr(p->attinmeta->tupdesc->attrs[i]->attname)) == 0)
            {
                p->cur_col = i;
                break;
            }
        }
        if (p->cur_col == NO_COLUMN) {
            elog(ERROR, "quasar_fdw internal: Couldnt find column for returned field: %s", s);
        }
    } else if (p->level > COLUMN_LEVEL) {
        if (is_json_type(p)) {
            jsonAppendCommaIf(p);
            appendStringInfo(&p->json, "\"%s\":", s);
        }
    }
    pfree(s);
    return YAJL_OK;
}

static int cb_start_map(void * ctx) {
    elog(DEBUG4, "entering function %s", __func__);
    parser *p = (parser*) ctx;
    int i;

    if (p->level == TOP_LEVEL && p->record_complete) {
        return YAJL_CANCEL;
    } else if (p->level == TOP_LEVEL) {
        /* Reset our values */
        for (i = 0; i < p->attinmeta->tupdesc->natts; ++i)
        {
            p->values[i] = PointerGetDatum(NULL);
            p->nulls[i] = true;
        }
    }

    if (p->level >= COLUMN_LEVEL) {
        if (is_json_type(p)) {
            jsonAppendCommaIf(p);
            appendStringInfoChar(&p->json, '{');
        } else if (warncheck(p)) {
            elog(WARNING, "quasar_fdw: column %s is scalar type but got json response.", NameStr(get_column(p)->attname));
        }
    }
    p->level++;
    return YAJL_OK;
}


static int cb_end_map(void * ctx) {
    elog(DEBUG4, "entering function %s", __func__);

    parser *p = (parser*) ctx;
    if (p->level > COLUMN_LEVEL) {
        if (is_json_type(p)) {
            appendStringInfoChar(&p->json, '}');
        }
    }

    p->level--;

    if (p->level == COLUMN_LEVEL) {
        if (p->json.len == 0) {
            store_null(p);
        } else {
            elog(DEBUG4, "Parsed value for json json: %s", p->json.data);
            store_datum(p, pstrdup(p->json.data), "%s");
            resetStringInfo(&p->json);
        }
    } else if (p->level == TOP_LEVEL) {
        p->record_complete = true;
    }
    return YAJL_OK;
}

static int cb_start_array(void * ctx) {
    parser *p = (parser*) ctx;
    if (p->level >= COLUMN_LEVEL) {
        if (is_array_type(p)) {
            arrayAppendCommaIf(p);
            appendStringInfoChar(&p->array, '{');
        } else if (is_json_type(p)) {
            jsonAppendCommaIf(p);
            appendStringInfoChar(&p->json, '[');
        }
    }
    p->level++;
    return YAJL_OK;
}

static int cb_end_array(void * ctx) {
    parser *p = (parser*) ctx;
    if (p->level > COLUMN_LEVEL) {
        if (is_array_type(p)) {
            appendStringInfoChar(&p->array, '}');
        } else if (is_json_type(p)) {
            appendStringInfo(&p->json, "]");
        } else if (warncheck(p)) {
            elog(WARNING, "quasar_fdw: column %s is scalar type but got json/array response.", NameStr(get_column(p)->attname));
        }
    }

    p->level--;

    if (p->level == COLUMN_LEVEL && is_json_type(p)) {
        if (p->json.len == 0)
            store_null(p);
        else {
            elog(DEBUG4, "Parsed value for deep json: %s", p->json.data);
            store_datum(p, pstrdup(p->json.data), "%s");
            resetStringInfo(&p->json);
        }
    } else if (p->level == COLUMN_LEVEL && is_array_type(p)) {
        if (p->array.len == 0) {
            store_null(p);
        } else {
            elog(DEBUG4, "Parsed value for deep array: %s", p->array.data);
            store_datum(p, pstrdup(p->array.data), "%s");
            resetStringInfo(&p->array);
        }
    }
    return YAJL_OK;
}

static yajl_callbacks callbacks = {
    cb_null,
    cb_boolean,
    NULL,
    NULL,
    cb_number,
    cb_string,
    cb_start_map,
    cb_map_key,
    cb_end_map,
    cb_start_array,
    cb_end_array
};

/* Yajl alloc functions use palloc */
void *yajl_palloc(void *ctx, size_t sz) {
    return palloc(sz);
}
void *yajl_repalloc(void *ctx, void *ptr, size_t sz) {
    if (ptr == NULL)
        return palloc(sz);
    else
        return repalloc(ptr, sz);
}
void yajl_pfree(void *ctx, void *ptr) {
    if (ptr != NULL)
        return pfree(ptr);
}
static yajl_alloc_funcs allocs = {yajl_palloc, yajl_repalloc, yajl_pfree, NULL};


void quasar_parse_alloc(quasar_parse_context *ctx, AttInMetadata *attinmeta) {
    elog(DEBUG4, "entering function %s", __func__);

    parser *p = palloc(sizeof(parser));
    p->cur_col = NO_COLUMN;
    p->level = TOP_LEVEL;
    p->record_complete = false;
    p->attinmeta = attinmeta;
    p->values = palloc(attinmeta->tupdesc->natts * sizeof(Datum));
    p->nulls = palloc(attinmeta->tupdesc->natts * sizeof(bool));
    p->warned = 0;
    initStringInfo(&p->json);
    initStringInfo(&p->array);
    ctx->p = p;
    ctx->handle = NULL;
    ctx->handle = yajl_alloc(&callbacks, &allocs, p);
    yajl_config(ctx->handle, yajl_allow_multiple_values, 1);
    yajl_parse(ctx->handle, NULL, 0); /* Allocate the lexer inside yajl */
}
void quasar_parse_free(quasar_parse_context *ctx) {
    elog(DEBUG4, "entering function %s", __func__);

    yajl_free(ctx->handle);
    pfree(((parser*)ctx->p)->json.data);
    pfree(((parser*)ctx->p)->array.data);
    pfree(((parser*)ctx->p)->values);
    pfree(((parser*)ctx->p)->nulls);
    pfree(ctx->p);
}

void quasar_parse_reset(quasar_parse_context *ctx) {
    elog(DEBUG4, "entering function %s", __func__);

    parser *p = (parser*) ctx->p;
    p->cur_col = NO_COLUMN;
    p->level = TOP_LEVEL;
    p->record_complete = false;
    p->warned = false;
    resetStringInfo(&p->json);
    resetStringInfo(&p->array);
    yajl_reset(ctx->handle);
}

bool
quasar_parse(quasar_parse_context *ctx,
             const char *buffer,
             size_t *buf_loc,
             size_t buf_size,
             HeapTuple *tuple)
{
    elog(DEBUG4, "entering function %s", __func__);

    bool found = false;
    parser *p = (parser*) ctx->p;
    yajl_status status;
    yajl_handle hand = ctx->handle;
    size_t bytes;

    if (*buf_loc < buf_size) {
        /* Response is formed as many json objects
         * So we can just parse until a full one is completed
         * and pick it up where we left off next time.
         * We do this by only parsing until
         * the beginning of the second object and stepping back one */

        status = yajl_parse(hand,
                            (const unsigned char *)buffer + *buf_loc,
                            buf_size - *buf_loc);
        bytes = yajl_get_bytes_consumed(hand);

        if (status == yajl_status_error) {
            unsigned char *errstr =
                yajl_get_error(hand, 1,
                               (const unsigned char *)buffer + *buf_loc,
                               buf_size - *buf_loc);
            elog(ERROR, "quasar_fdw internal: Error parsing json: %s", errstr);
        } else if (status == yajl_status_client_canceled) {
            bytes--;
            yajl_reset(ctx->handle);
        }

        elog(DEBUG3, "Consumed %lu bytes of json. %s record",
             bytes,
             p->record_complete ? "found" : "didnt find");

        found = p->record_complete;
        *buf_loc += bytes;
        p->record_complete = false;
    }

    if (found) {
        *tuple = heap_form_tuple(p->attinmeta->tupdesc, p->values, p->nulls);
    }

    return found;
}
