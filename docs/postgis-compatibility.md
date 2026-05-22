# PostGIS Compatibility

This project uses SpatiaLite as the spatial engine. Many PostGIS-style `ST_*` functions are already registered by SpatiaLite, and this fork adds a small set of compatibility aliases for common PostGIS names that map cleanly to existing SpatiaLite implementations.

These aliases are intended to make shared SQL easier across PostGIS and Android SpatiaLite. They are not a full PostgreSQL/PostGIS type-system clone.

## Compatibility Matrix

| PostGIS function | Status | Backing implementation | Notes |
| --- | --- | --- | --- |
| `ST_AsText(geom)` | Supported | `fnct_AsText` | Existing SpatiaLite registration. |
| `ST_AsBinary(geom)` | Supported | `fnct_AsBinary` | Existing SpatiaLite registration. |
| `ST_AsGeoJSON(geom[, precision[, options]])` | Added alias | `fnct_AsGeoJSON` | Maps to SpatiaLite `AsGeoJSON`. |
| `ST_GeomFromText(wkt[, srid])` | Supported | `fnct_GeomFromText*` | Existing SpatiaLite registration. |
| `ST_GeomFromWKB(wkb[, srid])` | Supported | `fnct_GeomFromWkb*` | Existing SpatiaLite registration. |
| `ST_GeomFromGeoJSON(json)` | Added alias | `fnct_FromGeoJSON` | Maps to SpatiaLite `GeomFromGeoJSON`. |
| `ST_GeomFromEWKT(ewkt)` | Supported | `fnct_FromEWKT` | Existing SpatiaLite registration. |
| `ST_AsEWKT(geom)` | Supported | `fnct_ToEWKT` | Existing SpatiaLite registration. |
| `ST_SRID(geom)` | Supported | `fnct_SRID` | Existing SpatiaLite registration. |
| `ST_SetSRID(geom, srid)` | Added alias | `fnct_SetSRID` | Maps to SpatiaLite `SetSRID`. |
| `ST_Transform(geom, srid)` | Supported | `fnct_Transform` | Requires PROJ support. |
| `ST_MakePoint(x, y)` | Added alias | `fnct_MakePoint1` | Returns XY point. |
| `ST_MakePoint(x, y, z)` | Added alias | `fnct_MakePointZ1` | PostGIS-style 3-argument form returns XYZ point. |
| `ST_MakePoint(x, y, z, m)` | Added alias | `fnct_MakePointZM1` | PostGIS-style 4-argument form returns XYZM point. |
| `ST_Point(x, y)` | Supported | `fnct_MakePoint1` | Existing SpatiaLite registration. |
| `ST_MakeLine(geom)` | Added alias | `fnct_MakeLine_step` / `fnct_MakeLine_final` | Aggregate form. |
| `ST_MakeLine(geom1, geom2)` | Added alias | `fnct_MakeLine` | Two-geometry form. |
| `ST_MakePolygon(shell[, holes])` | Supported | `fnct_MakePolygon` | Existing SpatiaLite registration. |
| `ST_MakeEnvelope(xmin, ymin, xmax, ymax[, srid])` | Added alias | `fnct_BuildMbr*` | Backed by SpatiaLite MBR geometry. |
| `ST_Envelope(geom)` | Supported | `fnct_Envelope` | Existing SpatiaLite registration. |
| `ST_XMin(geom)` | Added alias | `fnct_MbrMinX` | Alias for MBR minimum X. |
| `ST_YMin(geom)` | Added alias | `fnct_MbrMinY` | Alias for MBR minimum Y. |
| `ST_XMax(geom)` | Added alias | `fnct_MbrMaxX` | Alias for MBR maximum X. |
| `ST_YMax(geom)` | Added alias | `fnct_MbrMaxY` | Alias for MBR maximum Y. |
| `ST_X(point)` | Supported | `fnct_X` | Existing SpatiaLite registration. |
| `ST_Y(point)` | Supported | `fnct_Y` | Existing SpatiaLite registration. |
| `ST_Z(point)` | Supported | `fnct_Z` | Existing SpatiaLite registration. |
| `ST_M(point)` | Supported | `fnct_M` | Existing SpatiaLite registration. |
| `ST_IsEmpty(geom)` | Supported | `fnct_IsEmpty` | Existing SpatiaLite registration. |
| `ST_IsValid(geom)` | Supported | `fnct_IsValid` | Requires GEOS support. |
| `ST_IsValidReason(geom)` | Supported | `fnct_IsValidReason` | Requires GEOS support. |
| `ST_IsSimple(geom)` | Supported | `fnct_IsSimple` | Requires GEOS support. |
| `ST_Boundary(geom)` | Supported | `fnct_Boundary` | Requires GEOS support. |
| `ST_Centroid(geom)` | Supported | `fnct_Centroid` | Existing SpatiaLite registration. |
| `ST_PointOnSurface(geom)` | Supported | `fnct_PointOnSurface` | Existing SpatiaLite registration. |
| `ST_Buffer(geom, distance[, options])` | Supported | `fnct_Buffer` | Existing SpatiaLite registration. |
| `ST_Intersection(geom1, geom2)` | Supported | `fnct_Intersection` | Requires GEOS support. |
| `ST_Union(geom)` | Supported | `fnct_GUnion_step` / `fnct_GUnion_final` | Aggregate form. |
| `ST_Union(geom1, geom2)` | Supported | `fnct_GUnion` | Two-geometry form. |
| `ST_UnaryUnion(geom)` | Supported | `fnct_UnaryUnion` | Requires GEOS advanced support. |
| `ST_Difference(geom1, geom2)` | Supported | `fnct_Difference` | Requires GEOS support. |
| `ST_SymDifference(geom1, geom2)` | Supported | `fnct_SymDifference` | Requires GEOS support. |
| `ST_Intersects(geom1, geom2)` | Supported | `fnct_Intersects` | Requires GEOS support. |
| `ST_Contains(geom1, geom2)` | Supported | `fnct_Contains` | Requires GEOS support. |
| `ST_Within(geom1, geom2)` | Supported | `fnct_Within` | Requires GEOS support. |
| `ST_Touches(geom1, geom2)` | Supported | `fnct_Touches` | Requires GEOS support. |
| `ST_Crosses(geom1, geom2)` | Supported | `fnct_Crosses` | Requires GEOS support. |
| `ST_Overlaps(geom1, geom2)` | Supported | `fnct_Overlaps` | Requires GEOS support. |
| `ST_Equals(geom1, geom2)` | Supported | `fnct_Equals` | Requires GEOS support. |
| `ST_Disjoint(geom1, geom2)` | Supported | `fnct_Disjoint` | Requires GEOS support. |
| `ST_Covers(geom1, geom2)` | Supported | `fnct_Covers` | Existing SpatiaLite registration. |
| `ST_CoveredBy(geom1, geom2)` | Supported | `fnct_CoveredBy` | Existing SpatiaLite registration. |
| `ST_Distance(geom1, geom2[, use_ellipsoid])` | Supported | `fnct_Distance` | Existing SpatiaLite registration. |
| `ST_DWithin(geom1, geom2, distance[, use_spheroid])` | Added alias | `fnct_PtDistWithin` | Uses SpatiaLite distance-within behavior. For SRID 4326 single points, distance is meters. |
| `ST_Length(geom[, use_ellipsoid])` | Supported | `fnct_Length` | Existing SpatiaLite registration. |
| `ST_Perimeter(geom[, use_ellipsoid])` | Supported | `fnct_Perimeter` | Existing SpatiaLite registration. |
| `ST_Area(geom)` | Supported | `fnct_Area` | Existing SpatiaLite registration. |
| `ST_NumPoints(geom)` | Supported | `fnct_NumPoints` | Existing SpatiaLite registration. |
| `ST_NPoints(geom)` | Supported | `fnct_NPoints` | Existing SpatiaLite registration. |
| `ST_StartPoint(line)` | Supported | `fnct_StartPoint` | Existing SpatiaLite registration. |
| `ST_EndPoint(line)` | Supported | `fnct_EndPoint` | Existing SpatiaLite registration. |
| `ST_PointN(line, n)` | Supported | `fnct_PointN` | Existing SpatiaLite registration. |
| `ST_NumGeometries(geom)` | Supported | `fnct_NumGeometries` | Existing SpatiaLite registration. |
| `ST_GeometryN(geom, n)` | Supported | `fnct_GeometryN` | Existing SpatiaLite registration. |
| `ST_ExteriorRing(polygon)` | Supported | `fnct_ExteriorRing` | Existing SpatiaLite registration. |
| `ST_InteriorRingN(polygon, n)` | Supported | `fnct_InteriorRingN` | Existing SpatiaLite registration. |
| `ST_NumInteriorRing(polygon)` | Supported | `fnct_NumInteriorRings` | Existing SpatiaLite registration. |
| `ST_SnapToGrid(geom, ...)` | Supported | `fnct_SnapToGrid` | Existing SpatiaLite registration. |
| `ST_Snap(geom1, geom2, tolerance)` | Supported | `fnct_Snap` | Existing SpatiaLite registration. |
| `ST_Simplify(geom, tolerance)` | Supported | `fnct_Simplify` | Existing SpatiaLite registration. |
| `ST_SimplifyPreserveTopology(geom, tolerance)` | Supported | `fnct_SimplifyPreserveTopology` | Existing SpatiaLite registration. |
| `ST_LineMerge(geom)` | Supported | `fnct_LineMerge` | Existing SpatiaLite registration. |
| `ST_LineInterpolatePoint(line, fraction)` | Added alias | `fnct_LineInterpolatePoint` | Alias for `ST_Line_Interpolate_Point`. |
| `ST_LineLocatePoint(line, point)` | Added alias | `fnct_LineLocatePoint` | Alias for `ST_Line_Locate_Point`. |
| `ST_LineSubstring(line, start_fraction, end_fraction)` | Added alias | `fnct_LineSubstring` | Alias for `ST_Line_Substring`. |
| `ST_ClosestPoint(geom1, geom2)` | Supported | `fnct_ClosestPoint` | Existing SpatiaLite registration. |
| `ST_ShortestLine(geom1, geom2)` | Supported | `fnct_ShortestLine` | Existing SpatiaLite registration. |
| `ST_OffsetCurve(geom, distance)` | Supported | `fnct_OffsetCurve` | Existing SpatiaLite registration. |
| `ST_HausdorffDistance(geom1, geom2)` | Supported | `fnct_HausdorffDistance` | Existing SpatiaLite registration. |
| `ST_Reverse(geom)` | Supported | `fnct_Reverse` | Existing SpatiaLite registration. |
| `ST_Multi(geom)` | Supported | `fnct_CastToMulti` | Existing SpatiaLite registration. |
| `ST_Collect(geom)` | Supported | `fnct_Collect_step` / `fnct_Collect_final` | Aggregate form. |
| `ST_Collect(geom1, geom2)` | Supported | `fnct_Collect` | Two-geometry form. |

## Known Gaps

| PostGIS feature | Status | Reason |
| --- | --- | --- |
| `ST_Dump` / `ST_DumpPoints` | Not implemented | PostgreSQL set-returning records do not map directly to SQLite scalar functions. |
| Array forms such as `ST_Collect(geometry[])` | Not implemented | SQLite has no PostgreSQL array type. |
| `geography` type functions | Not implemented | SpatiaLite stores geometry BLOBs and does not implement the PostGIS geography type system. |
| Raster functions | Not implemented | PostGIS Raster is a separate subsystem. |
| Topology schema functions | Not implemented | PostGIS topology depends on PostgreSQL schemas and catalog behavior. |
| SFCGAL/3D advanced functions | Not implemented | Requires additional native dependencies not bundled here. |

## Testing

PostGIS compatibility aliases are covered by `PostgisCompatibilityTest`, which verifies runtime registration and representative SQL behavior against an in-memory database.
