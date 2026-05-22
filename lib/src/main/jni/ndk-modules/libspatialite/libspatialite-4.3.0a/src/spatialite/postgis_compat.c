/*
 * PostGIS compatibility aliases.
 *
 * This file is included by spatialite.c so it can reuse SpatiaLite's existing
 * static SQL function implementations without exporting additional symbols.
 */

static void
register_postgis_compat_sql_functions (sqlite3 * db,
				       struct splite_internal_cache *cache)
{
    sqlite3_create_function_v2 (db, "ST_AsGeoJSON", 1,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, cache,
				fnct_AsGeoJSON, 0, 0, 0);
    sqlite3_create_function_v2 (db, "ST_AsGeoJSON", 2,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, cache,
				fnct_AsGeoJSON, 0, 0, 0);
    sqlite3_create_function_v2 (db, "ST_AsGeoJSON", 3,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, cache,
				fnct_AsGeoJSON, 0, 0, 0);
    sqlite3_create_function_v2 (db, "ST_GeomFromGeoJSON", 1,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, cache,
				fnct_FromGeoJSON, 0, 0, 0);

    sqlite3_create_function_v2 (db, "ST_SetSRID", 2,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, cache,
				fnct_SetSRID, 0, 0, 0);

    sqlite3_create_function_v2 (db, "ST_XMin", 1,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0,
				fnct_MbrMinX, 0, 0, 0);
    sqlite3_create_function_v2 (db, "ST_YMin", 1,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0,
				fnct_MbrMinY, 0, 0, 0);
    sqlite3_create_function_v2 (db, "ST_XMax", 1,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0,
				fnct_MbrMaxX, 0, 0, 0);
    sqlite3_create_function_v2 (db, "ST_YMax", 1,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0,
				fnct_MbrMaxY, 0, 0, 0);

    sqlite3_create_function_v2 (db, "ST_MakeEnvelope", 4,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0,
				fnct_BuildMbr1, 0, 0, 0);
    sqlite3_create_function_v2 (db, "ST_MakeEnvelope", 5,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0,
				fnct_BuildMbr2, 0, 0, 0);

    sqlite3_create_function_v2 (db, "ST_MakePoint", 2,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0,
				fnct_MakePoint1, 0, 0, 0);
    sqlite3_create_function_v2 (db, "ST_MakePoint", 3,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0,
				fnct_MakePointZ1, 0, 0, 0);
    sqlite3_create_function_v2 (db, "ST_MakePoint", 4,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0,
				fnct_MakePointZM1, 0, 0, 0);

    sqlite3_create_function_v2 (db, "ST_MakeLine", 1,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, cache, 0,
				fnct_MakeLine_step, fnct_MakeLine_final, 0);
    sqlite3_create_function_v2 (db, "ST_MakeLine", 2,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, cache,
				fnct_MakeLine, 0, 0, 0);

    sqlite3_create_function_v2 (db, "ST_DWithin", 3,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, cache,
				fnct_PtDistWithin, 0, 0, 0);
    sqlite3_create_function_v2 (db, "ST_DWithin", 4,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, cache,
				fnct_PtDistWithin, 0, 0, 0);

    sqlite3_create_function_v2 (db, "ST_LineInterpolatePoint", 2,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, cache,
				fnct_LineInterpolatePoint, 0, 0, 0);
    sqlite3_create_function_v2 (db, "ST_LineLocatePoint", 2,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, cache,
				fnct_LineLocatePoint, 0, 0, 0);
    sqlite3_create_function_v2 (db, "ST_LineSubstring", 3,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, cache,
				fnct_LineSubstring, 0, 0, 0);
}
