/*
 * android_database_ImportFast.cpp -- JNI bridge for fast batch import
 */

#include <jni.h>
#include <string.h>
#include <stdlib.h>

#include "sqlite3.h"
#include "JNIHelp.h"
#include "ALog-priv.h"

#include <spatialite.h>

extern "C" {
#include "import_fast.h"
}

#define LOG_TAG "ImportFast"

struct ImportDbHandle {
    sqlite3* db;
    void* spatialiteCache;
};

static jlong nativeOpenImportDb(JNIEnv* env, jclass clazz, jstring dbPathStr)
{
    if (!dbPathStr) {
        jniThrowException(env, "java/lang/IllegalArgumentException", "dbPath is null");
        return 0;
    }
    const char* dbPath = env->GetStringUTFChars(dbPathStr, NULL);
    sqlite3* db = NULL;
    int rc = sqlite3_open_v2(dbPath, &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, NULL);
    env->ReleaseStringUTFChars(dbPathStr, dbPath);
    if (rc != SQLITE_OK) {
        if (db) sqlite3_close(db);
        jniThrowException(env, "org/spatialite/database/SQLiteException", "Failed to open database");
        return 0;
    }
    void* cache = spatialite_alloc_connection();
    spatialite_init_ex(db, cache, 0);

    ImportDbHandle* handle = (ImportDbHandle*)malloc(sizeof(ImportDbHandle));
    if (!handle) {
        spatialite_cleanup_ex(cache);
        sqlite3_close(db);
        return 0;
    }
    handle->db = db;
    handle->spatialiteCache = cache;
    return reinterpret_cast<jlong>(handle);
}

static void nativeCloseImportDb(JNIEnv* env, jclass clazz, jlong dbHandle)
{
    ImportDbHandle* handle = reinterpret_cast<ImportDbHandle*>(dbHandle);
    if (!handle) return;
    if (handle->spatialiteCache) spatialite_cleanup_ex(handle->spatialiteCache);
    if (handle->db) sqlite3_close(handle->db);
    free(handle);
}

static jlong nativeBeginImport(JNIEnv* env, jclass clazz,
    jlong dbHandle, jstring tableNameStr,
    jboolean hasProperties, jint geometrySrid, jint transactionBatch)
{
    ImportDbHandle* handle = reinterpret_cast<ImportDbHandle*>(dbHandle);
    if (!handle || !handle->db) return 0;

    const char* tableName = env->GetStringUTFChars(tableNameStr, NULL);
    void* ctx = import_fast_begin(handle->db, tableName,
        hasProperties ? 1 : 0, (int)geometrySrid, (int)transactionBatch);
    env->ReleaseStringUTFChars(tableNameStr, tableName);
    return reinterpret_cast<jlong>(ctx);
}

static jint nativeInsertFeature(JNIEnv* env, jclass clazz,
    jlong importHandle,
    jstring nameStr, jstring propertiesStr,
    jdouble mbrWidth, jdouble mbrHeight,
    jbyteArray wkbArray)
{
    void* handle = reinterpret_cast<void*>(importHandle);
    if (!handle) return -1;

    const char* name = nameStr ? env->GetStringUTFChars(nameStr, NULL) : NULL;
    int nameLen = name ? (int)strlen(name) : 0;
    const char* properties = propertiesStr ? env->GetStringUTFChars(propertiesStr, NULL) : NULL;
    int propsLen = properties ? (int)strlen(properties) : 0;

    jbyte* wkbData = NULL;
    int wkbLen = 0;
    if (wkbArray) {
        wkbLen = env->GetArrayLength(wkbArray);
        wkbData = env->GetByteArrayElements(wkbArray, NULL);
    }

    int rc = import_fast_insert(handle,
        name, nameLen, properties, propsLen,
        (double)mbrWidth, (double)mbrHeight,
        (const unsigned char*)wkbData, wkbLen);

    if (wkbData) env->ReleaseByteArrayElements(wkbArray, wkbData, JNI_ABORT);
    if (name) env->ReleaseStringUTFChars(nameStr, name);
    if (properties) env->ReleaseStringUTFChars(propertiesStr, properties);
    return rc;
}

static jlong nativeInsertFeatureWithId(JNIEnv* env, jclass clazz,
    jlong importHandle,
    jstring nameStr,
    jdouble mbrWidth, jdouble mbrHeight,
    jbyteArray wkbArray)
{
    void* handle = reinterpret_cast<void*>(importHandle);
    if (!handle) return -1;

    const char* name = nameStr ? env->GetStringUTFChars(nameStr, NULL) : NULL;
    int nameLen = name ? (int)strlen(name) : 0;

    jbyte* wkbData = NULL;
    int wkbLen = 0;
    if (wkbArray) {
        wkbLen = env->GetArrayLength(wkbArray);
        wkbData = env->GetByteArrayElements(wkbArray, NULL);
    }

    long long rowId = import_fast_insert_with_id(handle,
        name, nameLen,
        (double)mbrWidth, (double)mbrHeight,
        (const unsigned char*)wkbData, wkbLen);

    if (wkbData) env->ReleaseByteArrayElements(wkbArray, wkbData, JNI_ABORT);
    if (name) env->ReleaseStringUTFChars(nameStr, name);
    return (jlong)rowId;
}

static jint nativeEndImport(JNIEnv* env, jclass clazz, jlong importHandle)
{
    void* handle = reinterpret_cast<void*>(importHandle);
    if (!handle) return -1;
    return import_fast_end(handle);
}

static jint nativeGetImportCount(JNIEnv* env, jclass clazz, jlong importHandle)
{
    void* handle = reinterpret_cast<void*>(importHandle);
    if (!handle) return 0;
    return import_fast_get_count(handle);
}

static jstring nativeGetImportError(JNIEnv* env, jclass clazz, jlong importHandle)
{
    void* handle = reinterpret_cast<void*>(importHandle);
    if (!handle) return NULL;
    const char* msg = import_fast_get_error(handle);
    if (!msg) return NULL;
    return env->NewStringUTF(msg);
}

/* Properties batch helpers */
static jlong nativeBeginPropsImport(JNIEnv* env, jclass clazz,
    jlong dbHandle, jstring tableNameStr)
{
    ImportDbHandle* handle = reinterpret_cast<ImportDbHandle*>(dbHandle);
    if (!handle || !handle->db) return 0;
    const char* tableName = tableNameStr ? env->GetStringUTFChars(tableNameStr, NULL) : "feature_properties";
    void* ctx = import_fast_props_begin(handle->db, tableName);
    if (tableNameStr) env->ReleaseStringUTFChars(tableNameStr, tableName);
    return reinterpret_cast<jlong>(ctx);
}

static jint nativeInsertProps(JNIEnv* env, jclass clazz,
    jlong propsHandle, jlong featureId, jstring propertiesStr)
{
    void* handle = reinterpret_cast<void*>(propsHandle);
    if (!handle) return -1;
    const char* props = propertiesStr ? env->GetStringUTFChars(propertiesStr, NULL) : "{}";
    int propsLen = (int)strlen(props);
    int rc = import_fast_props_insert(handle, (long long)featureId, props, propsLen);
    if (propertiesStr) env->ReleaseStringUTFChars(propertiesStr, props);
    return rc;
}

static void nativeEndPropsImport(JNIEnv* env, jclass clazz, jlong propsHandle)
{
    void* handle = reinterpret_cast<void*>(propsHandle);
    if (handle) import_fast_props_end(handle);
}

/* Execute arbitrary SQL (for PRAGMAs, CREATE TABLE, etc.) */
static jint nativeExecSql(JNIEnv* env, jclass clazz, jlong dbHandle, jstring sqlStr)
{
    ImportDbHandle* handle = reinterpret_cast<ImportDbHandle*>(dbHandle);
    if (!handle || !handle->db || !sqlStr) return -1;
    const char* sql = env->GetStringUTFChars(sqlStr, NULL);
    int rc = sqlite3_exec(handle->db, sql, NULL, NULL, NULL);
    env->ReleaseStringUTFChars(sqlStr, sql);
    return rc == SQLITE_OK ? 0 : rc;
}

static JNINativeMethod sImportFastMethods[] = {
    { "nativeOpenImportDb", "(Ljava/lang/String;)J", (void*)nativeOpenImportDb },
    { "nativeCloseImportDb", "(J)V", (void*)nativeCloseImportDb },
    { "nativeBeginImport", "(JLjava/lang/String;ZII)J", (void*)nativeBeginImport },
    { "nativeInsertFeature", "(JLjava/lang/String;Ljava/lang/String;DD[B)I", (void*)nativeInsertFeature },
    { "nativeInsertFeatureWithId", "(JLjava/lang/String;DD[B)J", (void*)nativeInsertFeatureWithId },
    { "nativeEndImport", "(J)I", (void*)nativeEndImport },
    { "nativeGetImportCount", "(J)I", (void*)nativeGetImportCount },
    { "nativeGetImportError", "(J)Ljava/lang/String;", (void*)nativeGetImportError },
    { "nativeBeginPropsImport", "(JLjava/lang/String;)J", (void*)nativeBeginPropsImport },
    { "nativeInsertProps", "(JJLjava/lang/String;)I", (void*)nativeInsertProps },
    { "nativeEndPropsImport", "(J)V", (void*)nativeEndPropsImport },
    { "nativeExecSql", "(JLjava/lang/String;)I", (void*)nativeExecSql },
};

extern "C" int register_android_database_ImportFast(JNIEnv *env)
{
    return jniRegisterNativeMethods(env,
        "org/spatialite/database/ImportFast",
        sImportFastMethods, 12);
}