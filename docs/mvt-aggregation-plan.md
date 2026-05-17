# MVT 聚合实现方案

本文档记录 android-spatialite 的 MVT 聚合能力设计。目标是在 Android 端直接从 SpatiaLite 查询结果生成单个 Mapbox Vector Tile blob，支持一个瓦片包含多条 feature。

## 目标

第一阶段实现 geometry-only 聚合：

- 输入多行 `AsMVTGeom(...)` 结果。
- 输出一个完整 MVT v2 tile blob。
- 支持单 layer。
- 支持自定义 layer name。
- 支持自定义 extent。
- 暂不编码属性字典。
- 暂不编码 feature id。
- 暂不支持多 layer 合并。

## SQL API

保留坐标转换函数：

```sql
AsMVTGeom(geom, minx, miny, maxx, maxy)
AsMVTGeom(geom, minx, miny, maxx, maxy, extent)
AsMVTGeom(geom, minx, miny, maxx, maxy, extent, buffer)
AsMVTGeom(geom, minx, miny, maxx, maxy, extent, buffer, clip)
```

正式聚合函数：

```sql
AsMVT(mvt_geom)
AsMVT(mvt_geom, layer_name)
AsMVT(mvt_geom, layer_name, extent)
```

`AsMVT` 定义为聚合函数。每一行输入一个 `AsMVTGeom(...)` 结果，最终返回一个完整 MVT v2 tile blob。输入结果集只有一行时，输出即为单 feature tile；输入多行时，输出 multi-feature tile。

SQLite 不建议把同名同参数函数同时注册为 scalar 和 aggregate。当前实现只注册 aggregate 版本，用“一行输入”覆盖单条场景。

## 使用示例

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

返回值是一个 `BLOB`，即完整 MVT tile。

## Native 实现

SQLite 聚合函数使用 `sqlite3_create_function_v2` 注册：

```c
sqlite3_create_function_v2(db, "AsMVT", -1, SQLITE_UTF8,
                           0, 0, fnct_AsMVT_step, fnct_AsMVT_final, 0);
```

实现分为两部分：

```text
fnct_AsMVT_step:
    每处理一行，读取 mvt_geom blob。
    解析为 SpatiaLite geometry。
    编码成 MVT Feature message。
    追加到聚合上下文中的 features buffer。

fnct_AsMVT_final:
    创建 MVT Layer message。
    写入 layer name。
    写入全部 feature message。
    写入 extent。
    写入 version = 2。
    返回 Tile message blob。
```

聚合上下文：

```c
struct mvt_ctx {
    char *layer_name;
    int extent;
    struct mvt_buf features;
    int feature_count;
    int error;
};
```

第一阶段把所有 feature protobuf bytes 连续追加到 `features` buffer，避免额外链表结构。`final` 阶段一次性写入 layer。

## MVT 编码范围

第一阶段编码以下字段：

```text
Tile.layers = repeated Layer
Layer.name = string
Layer.features = repeated Feature
Layer.extent = uint32
Layer.version = uint32, fixed value 2
Feature.type = GeomType
Feature.geometry = packed uint32 commands
```

暂不编码：

```text
Layer.keys
Layer.values
Feature.id
Feature.tags
```

## 后续阶段

第二阶段：feature id。

```sql
AsMVT(mvt_geom, layer_name, extent, feature_id)
```

第三阶段：属性字典。

```sql
AsMVT(mvt_geom, layer_name, extent, json_props)
```

示例：

```sql
SELECT AsMVT(
    AsMVTGeom(geom, :minx, :miny, :maxx, :maxy, 4096),
    'features',
    4096,
    json_object('name', name, 'kind', kind)
)
FROM features
WHERE ...;
```

第四阶段：多 layer 合并。

可新增函数接收多个 layer blob，或在业务层拼接多个聚合结果。

## 当前限制

- 聚合结果为单 layer。
- feature 无属性。
- feature 无 id。
- `AsMVTGeom` 第一版仅做坐标转换和 bbox 粗过滤，复杂 polygon 裁剪、ring 方向修正、简化和退化几何过滤需要后续增强。
