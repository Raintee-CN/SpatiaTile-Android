package org.spatialite;

import android.database.Cursor;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.filters.SmallTest;
import androidx.test.platform.app.InstrumentationRegistry;

import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import android.database.sqlite.SQLiteException;

import org.spatialite.database.MvtFast;
import org.spatialite.database.SQLiteDatabase;

import java.io.File;

import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

@RunWith(AndroidJUnit4.class)
public class MvtTileTest {

    private static final double ORIGIN = 20037508.342789244;
    private static final String POLYGON =
            "POLYGON((0 0,500000 0,500000 500000,0 500000,0 0))";
    private SQLiteDatabase database;

    @Before
    public void setUp() {
        database = SQLiteDatabase.openOrCreateDatabase(":memory:", null);
        assertNotNull(database);
        queryBlob("SELECT InitSpatialMetaData(1)");
        database.execSQL("CREATE TABLE features (id INTEGER PRIMARY KEY, properties TEXT)");
        queryBlob("SELECT AddGeometryColumn('features', 'geom', 3857, 'POLYGON', 'XY')");
        database.execSQL("INSERT INTO features(id, properties, geom) VALUES (1, '{\"name\":\"park\"}', "
                + "GeomFromText('" + POLYGON + "', 3857))");
    }

    @After
    public void tearDown() {
        if (database != null) {
            database.close();
        }
    }

    @SmallTest
    @Test
    public void asMvtAndAsMvtFastReturnTiles() {
        byte[] legacy = queryBlob("SELECT AsMVT("
                + "AsMVTGeom(geom, " + -ORIGIN + ", " + -ORIGIN + ", " + ORIGIN + ", " + ORIGIN + ", 4096, 64, 1),"
                + "'features', 4096, properties, id, 64) FROM features");
        byte[] fast = queryBlob("SELECT AsMVTFast(geom, 'features', 4096, "
                + -ORIGIN + ", " + -ORIGIN + ", " + ORIGIN + ", " + ORIGIN
                + ", properties, id, 64) FROM features");
        assertTrue(legacy.length > 0);
        assertTrue(fast.length > 0);
    }

    @SmallTest
    @Test
    public void removedParallelSqlFunctionsAreNotRegistered() {
        assertFunctionMissing("SELECT AsMVT2(NULL)");
        assertFunctionMissing("SELECT AsMVTGeom2(NULL, 0, 0, 1, 1)");
    }

    @SmallTest
    @Test
    public void nativeGenerateTileReturnsPbf() {
        File dbFile = new File(InstrumentationRegistry.getInstrumentation().getTargetContext().getCacheDir(),
                "mvt-fast-test.sqlite");
        if (dbFile.exists()) {
            assertTrue(dbFile.delete());
        }
        SQLiteDatabase fileDb = SQLiteDatabase.openOrCreateDatabase(dbFile, null);
        try {
            rawQuery(fileDb, "SELECT InitSpatialMetaData(1)");
            fileDb.execSQL("CREATE TABLE features (id INTEGER PRIMARY KEY, properties TEXT)");
            rawQuery(fileDb, "SELECT AddGeometryColumn('features', 'geom', 3857, 'POLYGON', 'XY')");
            fileDb.execSQL("INSERT INTO features(id, properties, geom) VALUES (1, '{\"name\":\"park\"}', "
                    + "GeomFromText('" + POLYGON + "', 3857))");
            rawQuery(fileDb, "SELECT CreateSpatialIndex('features', 'geom')");
        } finally {
            fileDb.close();
        }

        long handle = MvtFast.nativeOpenDb(dbFile.getAbsolutePath());
        try {
            byte[] tile = MvtFast.nativeGenerateTile(
                    handle, "features", "features", 0, 0, 0,
                    "f.properties", null, 4096, 64, true, true);
            assertNotNull(tile);
            assertTrue(tile.length > 0);
            assertNull(MvtFast.nativeGenerateTile(
                    handle, "features", "features", 2, 0, 0,
                    "f.properties", null, 4096, 64, true, true));
        } finally {
            MvtFast.nativeCloseDb(handle);
            dbFile.delete();
        }
    }

    private void assertFunctionMissing(String sql) {
        try {
            database.rawQuery(sql, new Object[]{});
            fail(sql + " should not be registered");
        } catch (SQLiteException expected) {
            assertTrue(expected.getMessage().contains("no such function"));
        }
    }

    private byte[] queryBlob(String sql) {
        return rawQuery(database, sql);
    }

    private static byte[] rawQuery(SQLiteDatabase db, String sql) {
        Cursor cursor = db.rawQuery(sql, new Object[]{});
        try {
            assertTrue(cursor.moveToFirst());
            if (cursor.getType(0) != Cursor.FIELD_TYPE_BLOB) {
                return new byte[0];
            }
            return cursor.getBlob(0);
        } finally {
            cursor.close();
        }
    }
}
