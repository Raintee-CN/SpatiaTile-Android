/*
 * android_database_MvtFast.cpp -- JNI bridge for mvt_fast direct tile generation
 *
 * Provides its own database connection management for thread-safe tile generation.
 */

#include <jni.h>
#include <string.h>
#include <stdlib.h>

#include "sqlite3.h"
#include "JNIHelp.h"
#include "ALog-priv.h"

#include <spatialite.h>

extern "C" {
#include "mvt_fast.h"
}

#define LOG_TAG "MvtFast"

struct MvtDbHandle {
    sqlite3* db;
    void* spatialiteCache;
};

/*
 * nativeOpenDb: Open a read-only SpatiaLite connection for tile generation.
 */
static jlong nativeOpenDb(JNIEnv* env, jclass clazz, jstring dbPathStr)
{
    if (!dbPathStr) {
        jniThrowException(env, "java/lang/IllegalArgumentException", "dbPath is null");
        return 0;
    }

    const char* dbPath = env->GetStringUTFChars(dbPathStr, NULL);
    if (!dbPath) {
        jniThrowException(env, "java/lang/IllegalArgumentException", "Failed to get dbPath string");
        return 0;
    }

    sqlite3* db = NULL;
    int rc = sqlite3_open_v2(dbPath, &db,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL);
    env->ReleaseStringUTFChars(dbPathStr, dbPath);

    if (rc != SQLITE_OK) {
        if (db) sqlite3_close(db);
        jniThrowException(env, "org/spatialite/database/SQLiteException",
            "Failed to open database");
        return 0;
    }

    /* Initialize SpatiaLite */
    void* cache = spatialite_alloc_connection();
    spatialite_init_ex(db, cache, 0);

    /* Register mvt_fast SQL functions on this connection too */
    register_spatialite_mvt_fast_sql_functions(db);

    MvtDbHandle* handle = (MvtDbHandle*)malloc(sizeof(MvtDbHandle));
    if (!handle) {
        spatialite_cleanup_ex(cache);
        sqlite3_close(db);
        jniThrowException(env, "java/lang/OutOfMemoryError", "Failed to allocate handle");
        return 0;
    }
    handle->db = db;
    handle->spatialiteCache = cache;

    return reinterpret_cast<jlong>(handle);
}

/*
 * nativeCloseDb: Close a database handle.
 */
static void nativeCloseDb(JNIEnv* env, jclass clazz, jlong dbHandle)
{
    MvtDbHandle* handle = reinterpret_cast<MvtDbHandle*>(dbHandle);
    if (!handle) return;

    if (handle->spatialiteCache) {
        spatialite_cleanup_ex(handle->spatialiteCache);
    }
    if (handle->db) {
        sqlite3_close(handle->db);
    }
    free(handle);
}

/*
 * nativeGenerateTile: Generate MVT tile using direct C pipeline.
 */
static jbyteArray nativeGenerateTile(JNIEnv* env, jclass clazz,
    jlong dbHandle,
    jstring tableNameStr,
    jstring layerNameStr,
    jint z, jint x, jint y,
    jstring propertiesColumnStr,
    jstring propertiesJoinSqlStr,
    jint extent, jint buffer,
    jboolean hasSpatialIndex,
    jboolean useFeatureId)
{
    MvtDbHandle* handle = reinterpret_cast<MvtDbHandle*>(dbHandle);
    if (!handle || !handle->db) {
        jniThrowException(env, "java/lang/IllegalStateException",
            "Database handle is invalid or closed");
        return NULL;
    }

    if (!tableNameStr) {
        jniThrowException(env, "java/lang/IllegalArgumentException", "tableName is null");
        return NULL;
    }

    const char* tableName = env->GetStringUTFChars(tableNameStr, NULL);
    const char* layerName = layerNameStr ? env->GetStringUTFChars(layerNameStr, NULL) : NULL;
    const char* propertiesColumn = propertiesColumnStr ? env->GetStringUTFChars(propertiesColumnStr, NULL) : NULL;
    const char* propertiesJoinSql = propertiesJoinSqlStr ? env->GetStringUTFChars(propertiesJoinSqlStr, NULL) : NULL;

    unsigned char* outData = NULL;
    int outLen = 0;

    int rc = mvt_fast_generate_tile(
        handle->db,
        tableName,
        layerName ? layerName : "features",
        (int)z, (int)x, (int)y,
        propertiesColumn,
        propertiesJoinSql,
        extent > 0 ? (int)extent : 4096,
        buffer >= 0 ? (int)buffer : 64,
        (int)hasSpatialIndex,
        (int)useFeatureId,
        &outData, &outLen
    );

    env->ReleaseStringUTFChars(tableNameStr, tableName);
    if (layerName) env->ReleaseStringUTFChars(layerNameStr, layerName);
    if (propertiesColumn) env->ReleaseStringUTFChars(propertiesColumnStr, propertiesColumn);
    if (propertiesJoinSql) env->ReleaseStringUTFChars(propertiesJoinSqlStr, propertiesJoinSql);

    if (rc != 0 || !outData || outLen <= 0) {
        if (outData) free(outData);
        return NULL;
    }

    jbyteArray result = env->NewByteArray(outLen);
    if (result) {
        env->SetByteArrayRegion(result, 0, outLen, (jbyte*)outData);
    }
    free(outData);
    return result;
}

static JNINativeMethod sMvtFastMethods[] = {
    { "nativeOpenDb",
      "(Ljava/lang/String;)J",
      (void*)nativeOpenDb },
    { "nativeCloseDb",
      "(J)V",
      (void*)nativeCloseDb },
    { "nativeGenerateTile",
      "(JLjava/lang/String;Ljava/lang/String;IIILjava/lang/String;Ljava/lang/String;IIZZ)[B",
      (void*)nativeGenerateTile },
};

extern "C" int register_android_database_MvtFast(JNIEnv *env)
{
    return jniRegisterNativeMethods(env,
        "org/spatialite/database/MvtFast",
        sMvtFastMethods, 3);
}
