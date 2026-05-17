# android-spatialite 使用指南

本文档说明如何在 Android 项目中使用本库创建 SpatiaLite 数据库、写入空间数据、建立空间索引、批量导入数据，以及生成 MVT 矢量瓦片。

## 1. 引入模块

如果本库作为当前工程的 Gradle 子模块使用，在应用模块中添加依赖：

```gradle
dependencies {
    implementation project(':android-spatialite:lib')
}
```

如果单独使用 `android-spatialite` 工程，可将 `lib` 发布为 AAR 或直接作为 Gradle module 引入。

最低 Android 版本：`minSdkVersion 16`。

## 2. 基本 API

本库 API 基本对应 Android 原生 SQLite API。主要区别是包名：

```kotlin
import org.spatialite.database.SQLiteDatabase
import org.spatialite.database.SQLiteOpenHelper
```

游标仍使用 Android 标准游标：

```kotlin
import android.database.Cursor
```

不需要手动调用 `SQLiteDatabase.loadLibs()`。首次使用 `org.spatialite.database.SQLiteDatabase` 时会自动加载 native library。

## 3. 打开数据库

### 直接打开

```kotlin
val dbFile = context.getDatabasePath("map.db")
dbFile.parentFile?.mkdirs()

val db = SQLiteDatabase.openOrCreateDatabase(dbFile, null)
```

### 使用 SQLiteOpenHelper

```kotlin
class SpatialDbHelper(context: Context) : SQLiteOpenHelper(context, "map.db", null, 1) {
    override fun onCreate(db: SQLiteDatabase) {
        db.execSQL("SELECT InitSpatialMetaData(1)")

        db.execSQL("""
            CREATE TABLE features (
                id INTEGER PRIMARY KEY,
                name TEXT,
                kind TEXT
            )
        """.trimIndent())

        db.execSQL("SELECT AddGeometryColumn('features', 'geom', 3857, 'MULTIPOLYGON', 'XY')")
        db.execSQL("SELECT CreateSpatialIndex('features', 'geom')")
    }

    override fun onUpgrade(db: SQLiteDatabase, oldVersion: Int, newVersion: Int) {
        // 按业务需要迁移 schema。
    }
}
```

使用：

```kotlin
val db = SpatialDbHelper(context).writableDatabase
```

## 4. 初始化 SpatiaLite 元数据

新数据库第一次使用空间能力前需要初始化元数据：

```sql
SELECT InitSpatialMetaData(1);
```

建议只在新库创建时执行一次。可以通过检查 `geometry_columns` 是否存在判断是否已经初始化。

## 5. 创建空间表

推荐用普通表保存属性，再通过 `AddGeometryColumn` 添加几何列：

```sql
CREATE TABLE features (
    id INTEGER PRIMARY KEY,
    name TEXT,
    kind TEXT
);

SELECT AddGeometryColumn('features', 'geom', 3857, 'MULTIPOLYGON', 'XY');
SELECT CreateSpatialIndex('features', 'geom');
```

常用 SRID：

```text
4326: WGS84 经纬度
3857: Web Mercator，适合 Web 地图和 MVT 瓦片
```

几何类型示例：

```text
POINT
LINESTRING
POLYGON
MULTIPOINT
MULTILINESTRING
MULTIPOLYGON
GEOMETRY
```

## 6. 插入数据

### 插入 WKT

```kotlin
db.execSQL(
    """
    INSERT INTO features(id, name, kind, geom)
    VALUES (?, ?, ?, GeomFromText(?, 3857))
    """.trimIndent(),
    arrayOf<Any>(1, "A", "park", "MULTIPOLYGON(((0 0,10 0,10 10,0 10,0 0)))")
)
```

### 插入 WKB

如果业务侧已有 WKB，使用 `GeomFromWKB`：

```kotlin
val sql = """
    INSERT INTO features(id, name, kind, geom)
    VALUES (?, ?, ?, GeomFromWKB(?, 3857))
""".trimIndent()

db.execSQL(sql, arrayOf(id, name, kind, wkbBytes))
```

## 7. 查询空间数据

### 普通范围查询

```sql
SELECT id, name, kind, AsText(geom) AS wkt
FROM features
WHERE MbrIntersects(geom, BuildMbr(:minx, :miny, :maxx, :maxy, 3857));
```

### 使用 RTree 空间索引

创建空间索引后，推荐使用 `SpatialIndex` 虚表过滤 rowid：

```sql
SELECT f.id, f.name, f.kind, AsText(f.geom) AS wkt
FROM features AS f
WHERE f.rowid IN (
    SELECT rowid
    FROM SpatialIndex
    WHERE f_table_name = 'features'
      AND f_geometry_column = 'geom'
      AND search_frame = BuildMbr(:minx, :miny, :maxx, :maxy, 3857)
);
```

Kotlin 示例：

```kotlin
val cursor = db.rawQuery(
    """
    SELECT f.id, f.name, f.kind, AsText(f.geom) AS wkt
    FROM features AS f
    WHERE f.rowid IN (
        SELECT rowid
        FROM SpatialIndex
        WHERE f_table_name = 'features'
          AND f_geometry_column = 'geom'
          AND search_frame = BuildMbr(?, ?, ?, ?, 3857)
    )
    """.trimIndent(),
    arrayOf(minX, minY, maxX, maxY)
)

cursor.use {
    while (it.moveToNext()) {
        val id = it.getLong(0)
        val name = it.getString(1)
        val kind = it.getString(2)
        val wkt = it.getString(3)
    }
}
```

## 8. 批量导入数据

批量导入时不要逐行实时维护空间索引。推荐流程是：先关闭或暂不创建空间索引，事务内批量插入，导入完成后统一恢复空间索引。

### 新表批量导入

```kotlin
db.execSQL("PRAGMA journal_mode = WAL")
db.execSQL("PRAGMA synchronous = NORMAL")
db.execSQL("PRAGMA temp_store = MEMORY")
db.execSQL("PRAGMA cache_size = -200000")

db.execSQL("SELECT InitSpatialMetaData(1)")
db.execSQL("""
    CREATE TABLE features (
        id INTEGER PRIMARY KEY,
        name TEXT,
        kind TEXT
    )
""".trimIndent())
db.execSQL("SELECT AddGeometryColumn('features', 'geom', 3857, 'MULTIPOLYGON', 'XY')")

val stmt = db.compileStatement("""
    INSERT INTO features(id, name, kind, geom)
    VALUES (?, ?, ?, GeomFromWKB(?, 3857))
""".trimIndent())

db.beginTransactionNonExclusive()
try {
    for (item in items) {
        stmt.clearBindings()
        stmt.bindLong(1, item.id)
        stmt.bindString(2, item.name)
        stmt.bindString(3, item.kind)
        stmt.bindBlob(4, item.wkb)
        stmt.executeInsert()
    }

    db.setTransactionSuccessful()
} finally {
    db.endTransaction()
    stmt.close()
}

db.execSQL("SELECT CreateSpatialIndex('features', 'geom')")
db.execSQL("ANALYZE")
```

### 已有表追加导入

如果表已经存在且已经有空间索引，导入前禁用索引，导入后恢复：

```kotlin
db.execSQL("SELECT DisableSpatialIndex('features', 'geom')")

val stmt = db.compileStatement("""
    INSERT INTO features(id, name, kind, geom)
    VALUES (?, ?, ?, GeomFromWKB(?, 3857))
""".trimIndent())

db.beginTransactionNonExclusive()
try {
    for (item in items) {
        stmt.clearBindings()
        stmt.bindLong(1, item.id)
        stmt.bindString(2, item.name)
        stmt.bindString(3, item.kind)
        stmt.bindBlob(4, item.wkb)
        stmt.executeInsert()
    }

    db.setTransactionSuccessful()
} finally {
    db.endTransaction()
    stmt.close()
}

db.execSQL("SELECT RecoverSpatialIndex('features', 'geom')")
db.execSQL("ANALYZE")
```

导入建议：

- 使用 `compileStatement` 复用 SQL。
- 使用事务包裹批量写入。
- 大数据量可每 5,000 到 50,000 行提交一批，避免单事务过大。
- 普通 B-tree 索引和 RTree 空间索引都建议在导入完成后再创建。
- 导入完成后执行 `ANALYZE`，让查询优化器获得更准确统计信息。

## 9. 生成 MVT 矢量瓦片

本库新增了 MVT 辅助函数：

```sql
AsMVTGeom(geom, minx, miny, maxx, maxy)
AsMVTGeom(geom, minx, miny, maxx, maxy, extent)
AsMVTGeom(geom, minx, miny, maxx, maxy, extent, buffer)
AsMVTGeom(geom, minx, miny, maxx, maxy, extent, buffer, clip)

AsMVT(mvt_geom)
AsMVT(mvt_geom, layer_name)
AsMVT(mvt_geom, layer_name, extent)
```

### 参数说明

```text
geom: SpatiaLite geometry blob
minx, miny, maxx, maxy: 当前瓦片在数据坐标系中的范围
extent: MVT 内部坐标范围，默认建议 4096
buffer: 瓦片缓冲区，第一版保留参数
clip: 是否裁剪，第一版保留参数
layer_name: MVT layer 名称
```

当前第一版 `AsMVT` 输出 geometry-only MVT v2 PBF，不包含属性字典和 feature id。属性编码、多 layer、完整 polygon 裁剪和简化可后续扩展。

`AsMVT` 是聚合函数。每一行输入一个 `AsMVTGeom(...)` 结果，最终返回一个完整 MVT tile blob。输入结果集只有一行时，它自然输出只包含单条 feature 的 tile；因此单条和多条都使用同一个 `AsMVT` API。

SQLite 不建议把同名同参数函数同时注册为 scalar 和 aggregate。当前实现只注册 aggregate 版本，避免调用语义混乱。

### 聚合生成 MVT

```sql
SELECT AsMVT(
    AsMVTGeom(f.geom, :minx, :miny, :maxx, :maxy, 4096),
    'features',
    4096
) AS tile
FROM features AS f
WHERE f.rowid IN (
    SELECT rowid
    FROM SpatialIndex
    WHERE f_table_name = 'features'
      AND f_geometry_column = 'geom'
      AND search_frame = BuildMbr(:minx, :miny, :maxx, :maxy, 3857)
);
```

Kotlin 读取 MVT blob：

```kotlin
val cursor = db.rawQuery(sql, arrayOf(minX, minY, maxX, maxY))
val tile: ByteArray? = cursor.use {
    if (it.moveToFirst()) it.getBlob(0) else null
}
```

### 单条 geometry

如果只想生成单条 geometry 的 tile，可以让查询结果只返回一行：

```sql
SELECT AsMVT(
    AsMVTGeom(geom, :minx, :miny, :maxx, :maxy, 4096),
    'features',
    4096
) AS tile
FROM features
WHERE rowid IN (
    SELECT rowid
    FROM SpatialIndex
    WHERE f_table_name = 'features'
      AND f_geometry_column = 'geom'
      AND search_frame = BuildMbr(:minx, :miny, :maxx, :maxy, 3857)
)
LIMIT 1;
```

## 10. 坐标系与瓦片范围

如果数据用于 Web 地图，建议入库时统一为 EPSG:3857。MVT 查询时传入当前瓦片在 EPSG:3857 下的 bbox。

Web Mercator 全世界范围近似：

```text
min = -20037508.342789244
max =  20037508.342789244
```

瓦片 bbox 计算公式：

```kotlin
data class BBox(val minX: Double, val minY: Double, val maxX: Double, val maxY: Double)

fun webMercatorTileBounds(z: Int, x: Int, y: Int): BBox {
    val origin = 20037508.342789244
    val tiles = 1 shl z
    val size = origin * 2.0 / tiles

    val minX = -origin + x * size
    val maxX = minX + size
    val maxY = origin - y * size
    val minY = maxY - size

    return BBox(minX, minY, maxX, maxY)
}
```

## 11. 常用维护命令

```sql
-- 检查 SpatiaLite 版本
SELECT spatialite_version();

-- 检查 GEOS / PROJ 支持
SELECT geos_version();
SELECT proj4_version();

-- 创建空间索引
SELECT CreateSpatialIndex('features', 'geom');

-- 禁用空间索引
SELECT DisableSpatialIndex('features', 'geom');

-- 恢复空间索引
SELECT RecoverSpatialIndex('features', 'geom');

-- 更新统计信息
ANALYZE;

-- 数据库完整性检查
PRAGMA integrity_check;
```

## 12. 注意事项

- 空间表创建后，不要手动改 `geometry_columns`、RTree 表或空间索引触发器。
- 大批量导入时优先使用 WKB，WKT 解析成本更高。
- 查询范围和 geometry SRID 必须一致。
- `SpatialIndex` 只做 bbox 粗过滤；如果需要精确关系，继续叠加 `Intersects`、`Contains` 等空间函数。
- MVT 第一版主要用于验证 Android 端离线瓦片生成链路，生产级属性编码和复杂 polygon 处理需要继续增强。
