package org.spatialite;

import android.database.Cursor;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.filters.SmallTest;

import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.spatialite.database.SQLiteDatabase;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

@RunWith(AndroidJUnit4.class)
public class PostgisCompatibilityTest {

    private SQLiteDatabase database;

    @Before
    public void setUp() {
        database = SQLiteDatabase.openOrCreateDatabase(":memory:", null);
        assertNotNull(database);
    }

    @After
    public void tearDown() {
        database.close();
    }

    @SmallTest
    @Test
    public void testConstructorsAndSerializers() {
        assertScalarEquals("POINT(1 2)", "SELECT ST_AsText(ST_MakePoint(1, 2))");
        assertScalarEquals("POINT Z(1 2 3)", "SELECT ST_AsText(ST_MakePoint(1, 2, 3))");
        assertScalarEquals("POINT ZM(1 2 3 4)", "SELECT ST_AsText(ST_MakePoint(1, 2, 3, 4))");
        assertScalarEquals(4326, "SELECT ST_SRID(ST_SetSRID(ST_MakePoint(1, 2), 4326))");
        assertScalarEquals("POINT(1 2)", "SELECT ST_AsText(ST_GeomFromGeoJSON('{\"type\":\"Point\",\"coordinates\":[1,2]}'))");

        String geoJson = queryString("SELECT ST_AsGeoJSON(ST_MakePoint(1, 2))");
        assertTrue(geoJson.contains("\"Point\""));
        assertTrue(geoJson.contains("[1,2]"));
    }

    @SmallTest
    @Test
    public void testEnvelopeAndBoundsAliases() {
        assertScalarEquals(0.0, "SELECT ST_XMin(ST_MakeEnvelope(0, 1, 10, 11, 4326))");
        assertScalarEquals(1.0, "SELECT ST_YMin(ST_MakeEnvelope(0, 1, 10, 11, 4326))");
        assertScalarEquals(10.0, "SELECT ST_XMax(ST_MakeEnvelope(0, 1, 10, 11, 4326))");
        assertScalarEquals(11.0, "SELECT ST_YMax(ST_MakeEnvelope(0, 1, 10, 11, 4326))");
        assertScalarEquals(4326, "SELECT ST_SRID(ST_MakeEnvelope(0, 1, 10, 11, 4326))");
    }

    @SmallTest
    @Test
    public void testDistanceAndLineAliases() {
        assertScalarEquals(1, "SELECT ST_DWithin(ST_MakePoint(0, 0), ST_MakePoint(1, 1), 2)");
        assertScalarEquals(0, "SELECT ST_DWithin(ST_MakePoint(0, 0), ST_MakePoint(3, 4), 4)");
        assertScalarEquals("POINT(5 0)", "SELECT ST_AsText(ST_LineInterpolatePoint(ST_GeomFromText('LINESTRING(0 0,10 0)'), 0.5))");
        assertScalarEquals(0.5, "SELECT ST_LineLocatePoint(ST_GeomFromText('LINESTRING(0 0,10 0)'), ST_MakePoint(5, 0))");
        assertScalarEquals("LINESTRING(2 0, 8 0)", "SELECT ST_AsText(ST_LineSubstring(ST_GeomFromText('LINESTRING(0 0,10 0)'), 0.2, 0.8))");
    }

    @SmallTest
    @Test
    public void testAggregateAlias() {
        assertScalarEquals("LINESTRING(0 0, 1 1)", "SELECT ST_AsText(ST_MakeLine(geom)) FROM ("
                + "SELECT ST_MakePoint(0, 0) AS geom UNION ALL "
                + "SELECT ST_MakePoint(1, 1) AS geom)");
    }

    private void assertScalarEquals(String expected, String sql) {
        assertEquals(expected, queryString(sql));
    }

    private void assertScalarEquals(int expected, String sql) {
        assertEquals(expected, queryInt(sql));
    }

    private void assertScalarEquals(double expected, String sql) {
        assertEquals(expected, queryDouble(sql), 0.0000001);
    }

    private String queryString(String sql) {
        Cursor cursor = database.rawQuery(sql, new Object[]{});
        try {
            assertEquals(1, cursor.getCount());
            cursor.moveToFirst();
            return cursor.getString(0);
        } finally {
            cursor.close();
        }
    }

    private int queryInt(String sql) {
        Cursor cursor = database.rawQuery(sql, new Object[]{});
        try {
            assertEquals(1, cursor.getCount());
            cursor.moveToFirst();
            return cursor.getInt(0);
        } finally {
            cursor.close();
        }
    }

    private double queryDouble(String sql) {
        Cursor cursor = database.rawQuery(sql, new Object[]{});
        try {
            assertEquals(1, cursor.getCount());
            cursor.moveToFirst();
            return cursor.getDouble(0);
        } finally {
            cursor.close();
        }
    }
}
