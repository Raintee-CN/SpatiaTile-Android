package org.spatialite.database;

/**
 * High-performance batch feature import via direct native C pipeline.
 * <p>
 * Uses a single prepared statement with loop binding, managing transactions
 * internally for maximum throughput. Avoids per-row JNI overhead of the
 * standard SQLiteStatement approach.
 * <p>
 * Usage:
 * <pre>
 *   long db = ImportFast.nativeOpenImportDb(dbPath);
 *   ImportFast.nativeExecSql(db, "PRAGMA synchronous = OFF");
 *   long ctx = ImportFast.nativeBeginImport(db, "features", true, 3857, 10000);
 *   for (feature : features) {
 *       ImportFast.nativeInsertFeature(ctx, name, props, mbrW, mbrH, wkb);
 *   }
 *   int total = ImportFast.nativeEndImport(ctx);
 *   ImportFast.nativeCloseImportDb(db);
 * </pre>
 */
public class ImportFast {

    static {
        System.loadLibrary("android_spatialite");
    }

    /** Open a database for import (read-write, SpatiaLite initialized). */
    public static native long nativeOpenImportDb(String dbPath);

    /** Close an import database handle. */
    public static native void nativeCloseImportDb(long dbHandle);

    /** Begin a batch import session. Returns import context handle. */
    public static native long nativeBeginImport(
            long dbHandle, String tableName,
            boolean hasProperties, int geometrySrid, int transactionBatch);

    /** Insert one feature (INLINE properties mode). Returns 0 on success. */
    public static native int nativeInsertFeature(
            long importHandle,
            String name, String properties,
            double mbrWidth, double mbrHeight,
            byte[] wkb);

    /** Insert one feature and return its rowid (for SEPARATE properties). */
    public static native long nativeInsertFeatureWithId(
            long importHandle,
            String name,
            double mbrWidth, double mbrHeight,
            byte[] wkb);

    /** End import, commit remaining rows. Returns total inserted or -1 on error. */
    public static native int nativeEndImport(long importHandle);

    /** Get current inserted count. */
    public static native int nativeGetImportCount(long importHandle);

    /** Get error message if import failed. */
    public static native String nativeGetImportError(long importHandle);

    /** Begin properties batch insert (SEPARATE mode). */
    public static native long nativeBeginPropsImport(long dbHandle, String tableName);

    /** Insert one property row. */
    public static native int nativeInsertProps(long propsHandle, long featureId, String properties);

    /** End properties batch. */
    public static native void nativeEndPropsImport(long propsHandle);

    /** Execute arbitrary SQL (PRAGMAs, CREATE TABLE, etc.). Returns 0 on success. */
    public static native int nativeExecSql(long dbHandle, String sql);
}