# MVT/MLT 专用瓦片发布服务方案

## 目标

设计一套专门用于发布 MVT 和 MLT 瓦片的本地服务：

- 不依赖单文件 SQLite 存储。
- 不依赖 PostgreSQL/PostGIS。
- 数据以本地文件、分片包、散列或自定义二进制格式存储。
- 瓦片数据和空间索引分离。
- 放弃大部分 SQL 查询能力。
- 专注 MVT/MLT 的发布速度、并发读取、低延迟和易部署。

这套系统不是通用空间数据库，而是专用 tile serving engine。

## 总体思路

系统分为两个阶段：

1. 离线构建阶段：导入源数据，做投影、简化、切片、索引构建、MVT/MLT 编码或预编码，生成本地文件存储结构。
2. 在线发布阶段：根据 `tileset/z/x/y/format` 直接命中文件、索引或 feature block，快速返回 `.mvt` 或 `.mlt`。

在线服务的核心 API：

```text
GET /tiles/{tileset}/{z}/{x}/{y}.mvt
GET /tiles/{tileset}/{z}/{x}/{y}.mlt
GET /tiles/{tileset}/{z}/{x}/{y}?format=mvt
GET /tiles/{tileset}/{z}/{x}/{y}?format=mlt
GET /metadata/{tileset}.json
GET /health
GET /stats
```

## 架构

```text
                    ┌────────────────────┐
                    │  原始空间数据       │
                    │  shp/geojson/fgb    │
                    │  parquet/pmtiles等  │
                    └─────────┬──────────┘
                              │
                              ▼
                    ┌────────────────────┐
                    │  离线构建器         │
                    │  ingest/build       │
                    └─────────┬──────────┘
                              │
          ┌───────────────────┼───────────────────┐
          ▼                   ▼                   ▼
┌─────────────────┐ ┌─────────────────┐ ┌─────────────────┐
│ Tile Blob Store │ │ Spatial Index   │ │ Metadata Store  │
│ 瓦片数据文件     │ │ 空间索引文件     │ │ 图层/字段/样式信息 │
└────────┬────────┘ └────────┬────────┘ └────────┬────────┘
         │                   │                   │
         └───────────────────┼───────────────────┘
                             ▼
                    ┌────────────────────┐
                    │  Tile Server        │
                    │  HTTP / gRPC        │
                    └─────────┬──────────┘
                              ▼
                    ┌────────────────────┐
                    │  MapLibre / Client  │
                    └────────────────────┘
```

## 推荐路线

建议采用混合架构：

```text
低 zoom / 热门图层：预生成 MVT/MLT
高 zoom / 冷门图层：FeatureBlock 动态生成
热点请求：动态生成后写入 tile cache
```

读取优先级：

```text
1. 查内存缓存
2. 查预生成 TilePack
3. 查磁盘动态缓存
4. 查 FeatureBlock + Spatial Index 动态生成
5. 返回响应并异步写缓存
```

## 方案 A：预生成瓦片优先

离线阶段直接生成目标 zoom 范围内的 MVT/MLT 瓦片，在线阶段只做索引查找和文件读取。

### 优点

- 在线速度最快。
- 请求路径短。
- 不需要每次请求访问空间索引。
- 适合 CDN、本地缓存、边缘部署。
- 空瓦片可以在索引中直接标记。
- 可以同时存储 MVT 和 MLT。

### 缺点

- 构建时间长。
- 存储空间较大。
- 数据更新需要重建局部瓦片或增量瓦片包。
- 动态过滤能力弱。

### 适用场景

- 底图。
- 行政区。
- 道路。
- 水系。
- POI。
- 更新频率不高、读取频率高的数据。

### 存储示例

```text
data/
  tilesets/
    roads/
      manifest.json
      index/
        tile_index.bin
        tile_index.cdx
      blobs/
        00/
          00000000.blob
          00000001.blob
        01/
          00000002.blob
      meta/
        schema.json
        stats.json
        layers.json
```

也可以按内容散列存储：

```text
data/
  tilesets/
    roads/
      manifest.json
      tiles/
        mvt/
          0a/
            0a7f...c2.mvt
          bf/
            bf18...91.mvt
        mlt/
          33/
            337b...dd.mlt
      index/
        zxy_to_hash.bin
```

在线读取路径：

```text
z/x/y -> tile_id -> offset/length 或 hash -> 读取 blob -> HTTP 返回
```

## 方案 B：FeatureBlock 动态生成

数据不预切成完整瓦片，而是按空间分块存储 feature，再通过空间索引查找相关 block，请求时编码 MVT/MLT。

### 优点

- 存储比全量预切片小。
- 数据更新比预切片灵活。
- 可支持有限属性过滤。
- 同一套 FeatureBlock 可以生成 MVT 和 MLT。

### 缺点

- 在线 CPU 开销更大。
- 延迟不如预生成。
- 编码器、索引、缓存需要做得足够好。

### 存储示例

```text
data/
  tilesets/
    parcels/
      manifest.json
      features/
        blocks/
          00/
            block_000001.fblk
            block_000002.fblk
      index/
        rtree.bin
        zorder.bin
        block_stats.bin
      dictionaries/
        strings.dict
        keys.dict
      meta/
        schema.json
```

在线读取路径：

```text
z/x/y -> bbox -> spatial index 查 block_ids -> 读取 FeatureBlock
      -> bbox 精过滤 -> 坐标转换 -> MVT/MLT 编码 -> 返回
```

## TilePack 存储格式

不建议一个瓦片一个小文件。大量小文件会拖慢文件系统，尤其是百万级或千万级瓦片规模。

推荐使用分片包加索引：

```text
tileset/
  manifest.json
  shards/
    z0-6/
      shard_0000.tspack
      shard_0001.tspack
    z7-10/
      shard_0000.tspack
    z11-14/
      shard_0000.tspack
  index/
    tile_index.bin
```

每个 `.tspack` 是 append-only blob pack：

```text
[tile blob][tile blob][tile blob][tile blob]...
```

索引保存：

```text
tile_id -> shard_id, offset, length, encoding, compression, flags
```

### TileEntry 示例

```c
struct TileEntry {
    uint8_t  z;
    uint32_t x;
    uint32_t y;
    uint16_t shard_id;
    uint64_t offset;
    uint32_t length;
    uint8_t  format;      // 1=mvt, 2=mlt
    uint8_t  compression; // 0=none, 1=gzip, 2=br, 3=zstd
    uint8_t  flags;       // empty, deleted, redirect, etc.
};
```

构建时建议按 `z, morton(x, y)` 排序，在线使用 mmap 索引加二分查找。

## Tile ID 和空间局部性

建议使用 Morton / Z-order 排列瓦片。

```text
tile_key = z + morton(x, y)
```

好处：

- 相邻瓦片在磁盘上更可能接近。
- 地图拖动时读取更连续。
- 更利于 OS page cache。
- 更适合 range prefetch。

也可以使用 packed z/x/y：

```text
tile_id = (z << 58) | morton(x, y)
```

如果需要实现简单，可以先用 `(z, x, y)` 三元组排序。

## 空间索引设计

动态生成或增量构建需要独立空间索引。

推荐两层索引。

### 第 1 层：Tile Coverage Index

记录每个 block 覆盖哪些 zoom/tile 范围。

```text
block_id -> minx, miny, maxx, maxy
block_id -> min_z, max_z
block_id -> estimated_feature_count
block_id -> geometry_type
```

用途：

- 快速判断一个 tile 可能命中哪些 block。
- 按 zoom 建不同索引。
- 动态生成前做粗过滤。

### 第 2 层：Block 内部 Feature Index

每个 block 内部保存 feature bbox 和 offset。

```text
block header:
  feature_count
  bbox
  offsets[]
  feature_bboxes[]
  feature_records[]
```

请求时：

```text
tile bbox -> 查 block ids -> 读 block -> block 内 bbox 精过滤 -> 编码
```

可选实现：

- FlatGeobuf 风格 packed R-tree。
- 自定义 packed Hilbert R-tree。
- S2 / H3 cell 映射。
- 每 zoom tile inverted index。

如果目标是瓦片发布，而不是任意空间查询，优先推荐 tile inverted index：

```text
z/x/y -> block_ids[]
```

或按 zoom 分文件：

```text
index/z10.idx
index/z11.idx
index/z12.idx
```

## FeatureBlock 格式

动态生成时，FeatureBlock 应避免在线解析复杂格式。离线阶段应把 geometry 预处理成接近瓦片编码所需的结构。

推荐结构：

```text
FeatureBlock {
  Header
  StringDictionary
  PropertyKeyDictionary
  FeatureIndex[]
  GeometryData[]
  PropertyData[]
}
```

FeatureIndex：

```c
struct FeatureIndex {
    uint64_t feature_id;
    float minx;
    float miny;
    float maxx;
    float maxy;
    uint32_t geometry_offset;
    uint32_t geometry_length;
    uint32_t properties_offset;
    uint32_t properties_length;
    uint8_t geometry_type;
};
```

不建议在线存取 WKB。WKB 解析成本偏高。

推荐内部 geometry 格式：

```text
geometry_type
part_count
ring_count / line_count
delta encoded coordinates
```

如果所有发布都基于 WebMercator，离线阶段统一投影到 EPSG:3857，并量化成整数坐标：

```text
int32 x = round((mercator_x + origin) * scale)
int32 y = round((origin - mercator_y) * scale)
```

在线阶段只做 tile extent 变换，不做 PROJ。

## MVT 编码策略

MVT 在线编码路径：

```text
feature geometry -> tile 坐标 -> clip -> simplify -> command stream -> PBF
```

优化重点：

- 坐标预投影到 EPSG:3857，在线不做 PROJ。
- 每个 zoom 可预简化 geometry，避免低 zoom 使用过大 geometry。
- 属性 key/value 字典化，减少字符串处理。
- PBF writer 使用 arena allocator 或 reusable buffer。
- 热门 key/value 建全局 dictionary。
- 空瓦片直接返回 204 或空 tile blob。
- gzip/br 结果预压缩或缓存压缩结果。

MVT 响应头：

```text
Content-Type: application/vnd.mapbox-vector-tile
Content-Encoding: gzip
Cache-Control: public, max-age=...
```

`Content-Encoding` 只在返回预压缩内容时设置。

## MLT 编码策略

MLT 和 MVT 的主要区别：

- MVT 是 feature-oriented PBF。
- MLT 偏列式 FeatureTable。
- MLT 更适合属性列、几何流和字典压缩。

MLT 不建议从 MVT 转换生成，而应从同一套 FeatureBlock 直接编码。

MLT 输出可以复用：

```text
feature ids column
geometry streams
property columns
string dictionary
presence bitmap
```

如果要主打 MLT 性能，存储层可提前准备列式数据：

```text
block/
  geometry_types.u32
  geometry_offsets.u32
  vertices.i32
  feature_ids.u64
  prop_name_1.values
  prop_name_1.present_bitmap
  prop_name_2.values
  prop_name_2.present_bitmap
```

推荐策略：

```text
MVT：动态生成可接受
MLT：优先预生成，动态生成只做 fallback
```

高级 MLT 压缩，如 FastPFOR、FSST、shared dictionary、vertex dictionary，建议优先放到离线构建阶段。

## 缓存体系

建议至少三层缓存。

### L1：内存缓存

```text
key = tileset + z + x + y + format + layer/version
value = encoded tile bytes
```

适合：

- 当前地图视窗附近。
- 热点瓦片。
- 动态生成结果。

淘汰策略可用 LRU、TinyLFU 或 ARC。

### L2：磁盘动态缓存

```text
cache/
  mvt/
    shard_0001.cachepack
  mlt/
    shard_0001.cachepack
```

动态生成后的 tile 写入 cache pack。

### L3：预生成 TilePack

只读构建产物，作为稳定基础数据源。

读取顺序：

```text
memory cache -> disk generated cache -> prebuilt TilePack -> dynamic block encode
```

## 构建流程

离线构建器建议设计成独立 CLI：

```bash
tilebuild create --config tileset.yaml
tilebuild ingest --source roads.fgb --tileset roads
tilebuild index --tileset roads
tilebuild build-mvt --tileset roads --minzoom 0 --maxzoom 14
tilebuild build-mlt --tileset roads --minzoom 0 --maxzoom 14
tilebuild serve --data ./data
```

配置文件示例：

```yaml
tileset: roads
projection: EPSG:3857
minzoom: 0
maxzoom: 14

layers:
  - name: roads
    source: ./source/roads.fgb
    geometry: line
    id: id
    fields:
      - class
      - name
      - level
    minzoom: 5
    maxzoom: 14
    simplify:
      5: 20
      8: 5
      12: 1

storage:
  mode: hybrid
  shard_size_mb: 256
  tile_order: morton
  compression:
    mvt: gzip
    mlt: none

cache:
  memory_mb: 512
  disk_gb: 20
```

## 在线请求路径

预生成 MVT 请求路径：

```text
HTTP request
  -> parse tileset/z/x/y/format
  -> validate zoom/x/y
  -> make tile_key
  -> check memory cache
  -> lookup tile_index mmap
  -> if empty return 204 or empty tile
  -> read shard offset/length
  -> optionally decompress or direct return precompressed bytes
  -> set headers
  -> response
```

目标延迟：

```text
内存命中：< 1 ms
mmap index + pack read：1-5 ms
动态生成：5-50 ms，取决于 feature 数量
```

## 压缩策略

MVT 通常建议 gzip 传输，但不要每次请求现压缩。

推荐：

```text
预生成 mvt.gz
请求 Accept-Encoding 支持 gzip 时直接返回
不支持 gzip 时返回未压缩版本，或动态解压
```

MLT 是否压缩取决于客户端支持和 MLT 自身编码策略。

manifest 中标记格式和压缩：

```json
{
  "formats": {
    "mvt": {
      "contentType": "application/vnd.mapbox-vector-tile",
      "compression": "gzip"
    },
    "mlt": {
      "contentType": "application/vnd.maplibre-vector-tile",
      "compression": "none"
    }
  }
}
```

## 版本和增量更新

建议采用不可变版本目录和原子切换：

```text
data/
  versions/
    2026-07-01T120000/
      tilesets/
        roads/
    2026-07-02T120000/
      tilesets/
        roads/
  current -> versions/2026-07-02T120000
```

瓦片 blob 可用内容 hash 去重：

```text
blob_hash = blake3(tile_bytes)
```

增量更新流程：

```text
1. 找到变化 feature 的 bbox
2. 计算受影响 z/x/y
3. 重新生成这些 tile
4. 写新 pack 或 delta pack
5. 更新 index version
6. 原版本继续可读
7. 原子切换 current
```

## Manifest 设计

每个 tileset 必须有 manifest：

```json
{
  "tileset": "roads",
  "version": "2026-07-01",
  "minzoom": 0,
  "maxzoom": 14,
  "bounds": [-180, -85.0511, 180, 85.0511],
  "projection": "EPSG:3857",
  "tileSize": 512,
  "extent": 4096,
  "formats": ["mvt", "mlt"],
  "layers": [
    {
      "id": "roads",
      "geometry": "line",
      "minzoom": 5,
      "maxzoom": 14,
      "fields": {
        "name": "string",
        "class": "string",
        "level": "int"
      }
    }
  ],
  "storage": {
    "type": "tilepack",
    "index": "index/tile_index.bin",
    "shards": "shards/"
  }
}
```

## 并发和技术栈

推荐 Rust 或 Go。

### Rust

优点：

- 内存安全。
- mmap、zero-copy、压缩、HTTP 性能好。
- 适合写自定义二进制格式和编码器。
- 可使用 `axum`、`hyper`、`tokio`、`memmap2`、`blake3`。

适合长期高性能核心引擎。

### Go

优点：

- 服务开发快。
- 并发简单。
- 部署方便。
- HTTP 生态成熟。

适合快速产品化服务。

### C++

优点：

- 极致性能。
- 可复用已有 C/C++ MVT/MLT 编码逻辑。

缺点：

- 工程复杂度更高。

推荐组合：

```text
构建器：Rust
服务端：Rust
存储：TilePack + mmap index
编码：Rust 实现，必要时 FFI 复用 C/C++ 编码器
```

## 模块拆分

建议拆成 5 个模块：

```text
tile-core
  z/x/y、bbox、morton、projection、extent transform

tile-pack
  shard pack writer/reader
  mmap index
  hash/dedup
  compression metadata

tile-build
  数据导入
  简化
  切片
  MVT/MLT 预生成
  index 构建

tile-encode
  MVT encoder
  MLT encoder
  property dictionary
  geometry clip/simplify

tile-server
  HTTP API
  cache
  metrics
  reload version
```

## MVP 建议

第一版不要同时做动态 FeatureBlock、完整空间索引、MLT 高级压缩和复杂过滤。

建议第一版闭环：

1. 支持导入 GeoJSON / FlatGeobuf / Shapefile。
2. 离线预生成 MVT。
3. 使用 TilePack + mmap index 存储。
4. HTTP 服务按 `z/x/y.mvt` 读取返回。
5. 支持空瓦片标记。
6. 支持 gzip 预压缩。
7. 支持 manifest。
8. 后续增加 MLT 预生成。
9. 再后续增加 FeatureBlock 动态 fallback。
10. 最后增加增量更新、分布式构建和高级 MLT 压缩。

第一版存储结构：

```text
tileset/
  manifest.json
  index.mvt.bin
  shards/
    0000.tpack
    0001.tpack
```

第一版请求路径：

```text
z/x/y -> mmap binary search -> read shard -> return bytes
```

这条路径已经可以明显快于 SQLite/PG 动态生成。

## 最终形态

最终建议形态：

```text
核心存储：TilePack，不用 SQLite，不用 PG
索引：mmap 二进制 tile index + 可选 tile inverted block index
瓦片：MVT/MLT 可预生成，也可动态生成
数据块：FeatureBlock，用于高 zoom 或增量动态生成
发布服务：Rust HTTP server
缓存：memory LRU + disk cachepack
更新：不可变版本 + 原子切换
```

最终读取路径：

```text
Request z/x/y
  -> Memory cache
  -> Prebuilt MVT/MLT TilePack
  -> Disk generated cache
  -> FeatureBlock dynamic encode
  -> Store cache
  -> Response
```

## 关键取舍

- 放弃 SQL 能力，换取更短请求路径和更高并发。
- 优先预生成热门瓦片，降低在线 CPU。
- FeatureBlock 只用于高 zoom、冷门数据和动态 fallback。
- MVT 可以支持动态编码，MLT 优先预生成。
- 索引和数据分离，索引用 mmap，数据用大分片包。
- 版本不可变，切换用原子指针，避免在线写破坏读路径。
