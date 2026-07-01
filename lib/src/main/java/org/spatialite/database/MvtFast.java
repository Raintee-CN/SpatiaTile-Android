package org.spatialite.database;

/**
 * High-performance MVT tile generation via direct native C pipeline.
 * <p>
 * This class provides a direct JNI bridge to the mvt_fast tile generator,
 * bypassing SQL aggregate functions for maximum performance.
 * <p>
 * Two modes of operation:
 * <ol>
 *   <li>Using an existing native connection pointer (for advanced use)</li>
 *   <li>Using a database file path (opens its own read-only connection with SpatiaLite)</li>
 * </ol>
 * <p>
 * Example usage with path:
 * <pre>
 *   long handle = MvtFast.nativeOpenDb("/path/to/data.sqlite");
 *   try {
 *       byte[] tile = MvtFast.nativeGenerateTile(
 *           handle,
 *           "features_z10",         // table name
 *           "features",             // layer name
 *           10, 512, 340,           // z, x, y
 *           "f.properties",         // properties column expression
 *           null,                   // properties join SQL (or null)
 *           4096,                   // extent
 *           64,                     // buffer
 *           true,                   // has spatial index
 *           true                    // use feature id
 *       );
 *   } finally {
 *       MvtFast.nativeCloseDb(handle);
 *   }
 * </pre>
 */
public class MvtFast {

    static {
        System.loadLibrary("android_spatialite");
    }

    /**
     * Open a database for tile generation.
     * Opens a read-only connection with SpatiaLite initialized.
     *
     * @param dbPath Path to the SQLite/SpatiaLite database file
     * @return Native handle (must be closed with nativeCloseDb)
     */
    public static native long nativeOpenDb(String dbPath);

    /**
     * Close a database handle opened with nativeOpenDb.
     *
     * @param dbHandle Handle returned by nativeOpenDb
     */
    public static native void nativeCloseDb(long dbHandle);

    /**
     * Generate an MVT tile directly from the native layer.
     *
     * @param dbHandle       Native database handle (from nativeOpenDb)
     * @param tableName      Source table name
     * @param layerName      MVT layer name (e.g. "features")
     * @param z              Tile zoom level
     * @param x              Tile column
     * @param y              Tile row
     * @param propertiesColumn SQL expression for properties column, or null
     * @param propertiesJoinSql JOIN clause for properties, or null
     * @param extent         Tile extent (typically 4096)
     * @param buffer         Tile buffer in tile units (typically 64)
     * @param hasSpatialIndex Whether the table has a SpatialIndex
     * @param useFeatureId   Whether to include feature IDs in the tile
     * @return PBF byte array, or null if tile is empty or error occurred
     */
    public static native byte[] nativeGenerateTile(
            long dbHandle,
            String tableName,
            String layerName,
            int z, int x, int y,
            String propertiesColumn,
            String propertiesJoinSql,
            int extent, int buffer,
            boolean hasSpatialIndex,
            boolean useFeatureId
    );
}
