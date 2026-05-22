# SpatiaTile Android

[![JitPack](https://jitpack.io/v/Raintee-CN/SpatiaTile-Android.svg)](https://jitpack.io/#Raintee-CN/SpatiaTile-Android)

Offline SpatiaLite vector tile engine for Android, with MVT and experimental MapLibre Tile support.

## Features

- Android library built around SpatiaLite, SQLite, GEOS, PROJ, and related native dependencies.
- 100% offline spatial storage and query support through familiar Android SQLite-style APIs.
- MVT v2 tile generation directly from local SpatiaLite geometry tables.
- Experimental MapLibre Tile (`MLT`) generation for MapLibre GL JS MLT vector sources.
- Supports point, line, polygon, and multi geometry tile output.
- Supports JSON scalar feature properties and optional feature ids.
- Includes line/polygon buffer clipping, degenerate geometry filtering, and polygon ring direction correction for tile output.

## Installation

Add JitPack to your root `settings.gradle` or root `build.gradle`.

For modern Android projects using `dependencyResolutionManagement`:

```gradle
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
        maven { url 'https://jitpack.io' }
    }
}
```

For older projects using `allprojects`:

```gradle
allprojects {
    repositories {
        google()
        mavenCentral()
        maven { url 'https://jitpack.io' }
    }
}
```

Add the library dependency to your app module:

```gradle
dependencies {
    implementation 'com.github.Raintee-CN.SpatiaTile-Android:lib:<VERSION>'
}
```

Use a GitHub release tag, branch name, or commit hash as `<VERSION>`. For example, after creating a GitHub release tag `v1.0.0`, use `v1.0.0`.

## Basic Usage

Use the SpatiaLite SQLite wrapper classes instead of Android platform SQLite classes:

```kotlin
import org.spatialite.database.SQLiteDatabase
import org.spatialite.database.SQLiteOpenHelper
```

Android standard cursors are used:

```kotlin
import android.database.Cursor
```

`SQLiteDatabase.loadLibs()` is not required. Native libraries are loaded automatically when `org.spatialite.database.SQLiteDatabase` is first used.

## PostGIS Compatibility SQL Functions

The native SpatiaLite build exposes many common PostGIS-style `ST_*` functions, including geometry constructors, serializers, predicates, overlays, measurements, and line helpers. This fork also adds compatibility aliases for frequently used PostGIS names backed by existing SpatiaLite implementations:

```sql
ST_AsGeoJSON(geom)
ST_AsGeoJSON(geom, precision)
ST_AsGeoJSON(geom, precision, options)
ST_GeomFromGeoJSON(json)
ST_SetSRID(geom, srid)
ST_MakeEnvelope(xmin, ymin, xmax, ymax)
ST_MakeEnvelope(xmin, ymin, xmax, ymax, srid)
ST_MakePoint(x, y)
ST_MakePoint(x, y, z)
ST_MakePoint(x, y, z, m)
ST_MakeLine(geom)
ST_MakeLine(geom1, geom2)
ST_DWithin(geom1, geom2, distance)
ST_DWithin(geom1, geom2, distance, use_spheroid)
ST_XMin(geom)
ST_YMin(geom)
ST_XMax(geom)
ST_YMax(geom)
ST_LineInterpolatePoint(line, fraction)
ST_LineLocatePoint(line, point)
ST_LineSubstring(line, start_fraction, end_fraction)
```

These are compatibility aliases, not a full PostgreSQL/PostGIS type-system clone. SQLite does not support PostgreSQL array arguments or set-returning records, so functions such as `ST_Dump` need separate SQLite-friendly designs.

## Vector Tile SQL Functions

### MVT

```sql
AsMVTGeom(geom, minx, miny, maxx, maxy)
AsMVTGeom(geom, minx, miny, maxx, maxy, extent)
AsMVTGeom(geom, minx, miny, maxx, maxy, extent, buffer)
AsMVTGeom(geom, minx, miny, maxx, maxy, extent, buffer, clip)

AsMVT(mvt_geom)
AsMVT(mvt_geom, layer_name)
AsMVT(mvt_geom, layer_name, extent)
AsMVT(mvt_geom, layer_name, extent, properties_json)
AsMVT(mvt_geom, layer_name, extent, properties_json, feature_id)
AsMVT(mvt_geom, layer_name, extent, properties_json, feature_id, buffer)

MVTConcat(tile_blob_1, tile_blob_2, ...)
```

Example:

```sql
SELECT AsMVT(
    AsMVTGeom(geom, :minx, :miny, :maxx, :maxy, 4096),
    'features',
    4096,
    properties_json,
    id,
    256
) AS tile
FROM features;
```

### MLT

```sql
AsMLT(mlt_geom)
AsMLT(mlt_geom, layer_name)
AsMLT(mlt_geom, layer_name, extent)
AsMLT(mlt_geom, layer_name, extent, properties_json)
AsMLT(mlt_geom, layer_name, extent, properties_json, feature_id)
ST_AsMLT(mlt_geom, layer_name, extent, properties_json, feature_id)
```

Example:

```sql
SELECT ST_AsMLT(
    AsMVTGeom(geom, :minx, :miny, :maxx, :maxy, 4096),
    'features',
    4096,
    properties_json,
    id
) AS tile
FROM features;
```

MLT output is experimental. It writes valid MapLibre Tile feature tables with geometry, optional feature ids, and nullable string property columns generated from top-level scalar values in `properties_json`. Advanced MLT compression such as FastPFOR, FSST, shared dictionaries, and vertex dictionaries is not implemented yet.

## Native Dependencies

- SQLite 3.15.1
- SpatiaLite 4.3.0a
- GEOS 3.4.2
- PROJ 4.8.0
- lzma 5.2.1
- iconv 1.13
- libxml2 2.9.2
- FreeXL 1.0.2

## Requirements

- Android min SDK 16
- Android NDK for local builds

## Documentation

See `docs/usage.md` for full Android, SpatiaLite, MVT, and MLT usage examples.

## License

Apache License 2.0
