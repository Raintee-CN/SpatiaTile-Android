/*
 import_fast.c -- High-performance batch feature import for android-spatialite

 Provides a C-layer batch import API that:
 1. Uses a single prepared statement with loop binding (avoids JNI per-row overhead)
 2. Manages transactions internally with configurable batch size
 3. Supports both INLINE and NO-properties modes
 4. Handles WKB geometry with optional Transform (4326→3857)
*/

#include <stdlib.h>
#include <string.h>
#include <math.h>

#if defined(_WIN32) && !defined(__MINGW32__)
#include "config-msvc.h"
#else
#include "config.h"
#endif

#include <spatialite/sqlite.h>
#include <spatialite/gaiageo.h>

/* ========================================================================
   Batch Import Context
   ======================================================================== */

#define IMPORT_DEFAULT_BATCH_SIZE 5000
#define IMPORT_MAX_SQL_SIZE 65536

struct import_ctx
{
    sqlite3 *db;
    sqlite3_stmt *insert_stmt;
    char *table_name;
    int has_properties;       /* 1 = INLINE properties column */
    int geometry_srid;        /* 4326 or 3857 */
    int transaction_batch;    /* rows per transaction commit */
    int rows_in_transaction;
    int total_inserted;
    int error;
    char error_msg[256];
};

static int
import_ctx_init (struct import_ctx *ctx, sqlite3 *db, const char *table_name,
                 int has_properties, int geometry_srid, int transaction_batch)
{
    char sql[1024];
    const char *geom_expr;
    int rc;

    memset (ctx, 0, sizeof (struct import_ctx));
    ctx->db = db;
    ctx->has_properties = has_properties;
    ctx->geometry_srid = geometry_srid;
    ctx->transaction_batch = transaction_batch > 0 ? transaction_batch : IMPORT_DEFAULT_BATCH_SIZE;

    ctx->table_name = (char *) malloc (strlen (table_name) + 1);
    if (!ctx->table_name)
      {
          ctx->error = 1;
          snprintf (ctx->error_msg, sizeof(ctx->error_msg), "Out of memory");
          return 1;
      }
    strcpy (ctx->table_name, table_name);

    /* Build INSERT SQL with prepared statement */
    if (geometry_srid == 3857)
        geom_expr = "GeomFromWKB(?, 3857)";
    else
        geom_expr = "Transform(GeomFromWKB(?, 4326), 3857)";

    if (has_properties)
        snprintf (sql, sizeof(sql),
            "INSERT INTO %s(name, properties, mbr_width, mbr_height, geom) "
            "VALUES (?, ?, ?, ?, %s)",
            table_name, geom_expr);
    else
        snprintf (sql, sizeof(sql),
            "INSERT INTO %s(name, mbr_width, mbr_height, geom) "
            "VALUES (?, ?, ?, %s)",
            table_name, geom_expr);

    rc = sqlite3_prepare_v2 (db, sql, -1, &ctx->insert_stmt, NULL);
    if (rc != SQLITE_OK)
      {
          ctx->error = 1;
          snprintf (ctx->error_msg, sizeof(ctx->error_msg),
                    "Prepare failed: %s", sqlite3_errmsg (db));
          return 1;
      }

    /* Begin first transaction */
    rc = sqlite3_exec (db, "BEGIN TRANSACTION", NULL, NULL, NULL);
    if (rc != SQLITE_OK)
      {
          ctx->error = 1;
          snprintf (ctx->error_msg, sizeof(ctx->error_msg),
                    "BEGIN failed: %s", sqlite3_errmsg (db));
          return 1;
      }
    ctx->rows_in_transaction = 0;
    return 0;
}

static void
import_ctx_free (struct import_ctx *ctx)
{
    if (ctx->insert_stmt)
        sqlite3_finalize (ctx->insert_stmt);
    ctx->insert_stmt = NULL;
    if (ctx->table_name)
        free (ctx->table_name);
    ctx->table_name = NULL;
}

static int
import_commit_transaction (struct import_ctx *ctx)
{
    int rc;
    rc = sqlite3_exec (ctx->db, "COMMIT", NULL, NULL, NULL);
    if (rc != SQLITE_OK)
      {
          ctx->error = 1;
          snprintf (ctx->error_msg, sizeof(ctx->error_msg),
                    "COMMIT failed: %s", sqlite3_errmsg (ctx->db));
          return 1;
      }
    rc = sqlite3_exec (ctx->db, "BEGIN TRANSACTION", NULL, NULL, NULL);
    if (rc != SQLITE_OK)
      {
          ctx->error = 1;
          snprintf (ctx->error_msg, sizeof(ctx->error_msg),
                    "BEGIN failed: %s", sqlite3_errmsg (ctx->db));
          return 1;
      }
    ctx->rows_in_transaction = 0;
    return 0;
}

/* ========================================================================
   Public API: Batch Insert Features
   ======================================================================== */

/*
 * import_fast_begin - Initialize batch import context.
 * Returns opaque handle, or NULL on error.
 */
void *
import_fast_begin (sqlite3 *db, const char *table_name,
                   int has_properties, int geometry_srid,
                   int transaction_batch)
{
    struct import_ctx *ctx = (struct import_ctx *) malloc (sizeof (struct import_ctx));
    if (!ctx)
        return NULL;
    if (import_ctx_init (ctx, db, table_name, has_properties, geometry_srid, transaction_batch))
      {
          import_ctx_free (ctx);
          free (ctx);
          return NULL;
      }
    return (void *) ctx;
}

/*
 * import_fast_insert - Insert a single feature into the batch.
 * Handles transaction commits internally.
 *
 * Returns 0 on success, non-zero on error.
 */
int
import_fast_insert (void *handle,
                    const char *name, int name_len,
                    const char *properties, int properties_len,
                    double mbr_width, double mbr_height,
                    const unsigned char *wkb, int wkb_len)
{
    struct import_ctx *ctx = (struct import_ctx *) handle;
    int rc;
    int param = 1;

    if (!ctx || ctx->error)
        return 1;

    sqlite3_reset (ctx->insert_stmt);
    sqlite3_clear_bindings (ctx->insert_stmt);

    /* Bind name */
    if (name && name_len > 0)
        sqlite3_bind_text (ctx->insert_stmt, param, name, name_len, SQLITE_STATIC);
    else
        sqlite3_bind_null (ctx->insert_stmt, param);
    param++;

    /* Bind properties (only if INLINE mode) */
    if (ctx->has_properties)
      {
          if (properties && properties_len > 0)
              sqlite3_bind_text (ctx->insert_stmt, param, properties, properties_len, SQLITE_STATIC);
          else
              sqlite3_bind_null (ctx->insert_stmt, param);
          param++;
      }

    /* Bind mbr_width, mbr_height */
    sqlite3_bind_double (ctx->insert_stmt, param++, mbr_width);
    sqlite3_bind_double (ctx->insert_stmt, param++, mbr_height);

    /* Bind WKB geometry */
    if (wkb && wkb_len > 0)
        sqlite3_bind_blob (ctx->insert_stmt, param, wkb, wkb_len, SQLITE_STATIC);
    else
        sqlite3_bind_null (ctx->insert_stmt, param);

    /* Execute */
    rc = sqlite3_step (ctx->insert_stmt);
    if (rc != SQLITE_DONE)
      {
          ctx->error = 1;
          snprintf (ctx->error_msg, sizeof(ctx->error_msg),
                    "INSERT failed row %d: %s", ctx->total_inserted, sqlite3_errmsg (ctx->db));
          return 1;
      }

    ctx->total_inserted++;
    ctx->rows_in_transaction++;

    /* Auto-commit at batch boundary */
    if (ctx->rows_in_transaction >= ctx->transaction_batch)
      {
          if (import_commit_transaction (ctx))
              return 1;
      }

    return 0;
}

/*
 * import_fast_insert_with_id - Insert and return the rowid (for SEPARATE properties).
 */
long long
import_fast_insert_with_id (void *handle,
                            const char *name, int name_len,
                            double mbr_width, double mbr_height,
                            const unsigned char *wkb, int wkb_len)
{
    struct import_ctx *ctx = (struct import_ctx *) handle;
    int rc;
    int param = 1;

    if (!ctx || ctx->error)
        return -1;

    sqlite3_reset (ctx->insert_stmt);
    sqlite3_clear_bindings (ctx->insert_stmt);

    /* Bind name */
    if (name && name_len > 0)
        sqlite3_bind_text (ctx->insert_stmt, param, name, name_len, SQLITE_STATIC);
    else
        sqlite3_bind_null (ctx->insert_stmt, param);
    param++;

    /* Skip properties (not INLINE) */
    if (ctx->has_properties)
      {
          sqlite3_bind_null (ctx->insert_stmt, param);
          param++;
      }

    /* Bind mbr_width, mbr_height */
    sqlite3_bind_double (ctx->insert_stmt, param++, mbr_width);
    sqlite3_bind_double (ctx->insert_stmt, param++, mbr_height);

    /* Bind WKB geometry */
    if (wkb && wkb_len > 0)
        sqlite3_bind_blob (ctx->insert_stmt, param, wkb, wkb_len, SQLITE_STATIC);
    else
        sqlite3_bind_null (ctx->insert_stmt, param);

    /* Execute */
    rc = sqlite3_step (ctx->insert_stmt);
    if (rc != SQLITE_DONE)
      {
          ctx->error = 1;
          snprintf (ctx->error_msg, sizeof(ctx->error_msg),
                    "INSERT failed row %d: %s", ctx->total_inserted, sqlite3_errmsg (ctx->db));
          return -1;
      }

    ctx->total_inserted++;
    ctx->rows_in_transaction++;

    if (ctx->rows_in_transaction >= ctx->transaction_batch)
      {
          if (import_commit_transaction (ctx))
              return -1;
      }

    return sqlite3_last_insert_rowid (ctx->db);
}

/*
 * import_fast_end - Finalize the batch import.
 * Commits remaining transaction, finalizes statement.
 * Returns total rows inserted, or -1 on error.
 */
int
import_fast_end (void *handle)
{
    struct import_ctx *ctx = (struct import_ctx *) handle;
    int total;
    int rc;

    if (!ctx)
        return -1;

    if (!ctx->error && ctx->rows_in_transaction > 0)
      {
          rc = sqlite3_exec (ctx->db, "COMMIT", NULL, NULL, NULL);
          if (rc != SQLITE_OK)
              ctx->error = 1;
      }
    else if (ctx->error)
      {
          sqlite3_exec (ctx->db, "ROLLBACK", NULL, NULL, NULL);
      }

    total = ctx->error ? -1 : ctx->total_inserted;
    import_ctx_free (ctx);
    free (ctx);
    return total;
}

/*
 * import_fast_get_error - Get error message (call before import_fast_end).
 */
const char *
import_fast_get_error (void *handle)
{
    struct import_ctx *ctx = (struct import_ctx *) handle;
    if (!ctx || !ctx->error)
        return NULL;
    return ctx->error_msg;
}

/*
 * import_fast_get_count - Get current inserted count.
 */
int
import_fast_get_count (void *handle)
{
    struct import_ctx *ctx = (struct import_ctx *) handle;
    if (!ctx)
        return 0;
    return ctx->total_inserted;
}

/* ========================================================================
   Batch Properties Insert (for SEPARATE mode)
   ======================================================================== */

struct import_props_ctx
{
    sqlite3 *db;
    sqlite3_stmt *stmt;
    int error;
    char error_msg[256];
};

void *
import_fast_props_begin (sqlite3 *db, const char *props_table)
{
    struct import_props_ctx *ctx;
    char sql[512];
    int rc;

    ctx = (struct import_props_ctx *) malloc (sizeof (struct import_props_ctx));
    if (!ctx)
        return NULL;
    memset (ctx, 0, sizeof (struct import_props_ctx));
    ctx->db = db;

    snprintf (sql, sizeof(sql),
        "INSERT INTO %s(feature_id, properties) VALUES (?, ?)", props_table);
    rc = sqlite3_prepare_v2 (db, sql, -1, &ctx->stmt, NULL);
    if (rc != SQLITE_OK)
      {
          free (ctx);
          return NULL;
      }
    return (void *) ctx;
}

int
import_fast_props_insert (void *handle, long long feature_id,
                          const char *properties, int properties_len)
{
    struct import_props_ctx *ctx = (struct import_props_ctx *) handle;
    int rc;

    if (!ctx || ctx->error)
        return 1;

    sqlite3_reset (ctx->stmt);
    sqlite3_bind_int64 (ctx->stmt, 1, feature_id);
    if (properties && properties_len > 0)
        sqlite3_bind_text (ctx->stmt, 2, properties, properties_len, SQLITE_STATIC);
    else
        sqlite3_bind_text (ctx->stmt, 2, "{}", 2, SQLITE_STATIC);

    rc = sqlite3_step (ctx->stmt);
    if (rc != SQLITE_DONE)
      {
          ctx->error = 1;
          snprintf (ctx->error_msg, sizeof(ctx->error_msg),
                    "Props INSERT failed: %s", sqlite3_errmsg (ctx->db));
          return 1;
      }
    return 0;
}

void
import_fast_props_end (void *handle)
{
    struct import_props_ctx *ctx = (struct import_props_ctx *) handle;
    if (!ctx)
        return;
    if (ctx->stmt)
        sqlite3_finalize (ctx->stmt);
    free (ctx);
}