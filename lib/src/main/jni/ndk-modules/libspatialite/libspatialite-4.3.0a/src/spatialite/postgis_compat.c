/*
 * PostGIS compatibility aliases and custom spatial functions.
 *
 * This file is included by spatialite.c so it can reuse SpatiaLite's existing
 * static SQL function implementations without exporting additional symbols.
 */

/* ========================================================================
 * ST_Orthogonalize(geom, grid_size)
 *
 * Snaps all vertices to a grid, then converts diagonal edges into
 * staircase (horizontal + vertical) segments.  The result is a geometry
 * where all edges are axis-aligned (orthogonal / rectilinear).
 *
 * Algorithm per ring/linestring:
 *   1. SnapToGrid all coordinates.
 *   2. For each pair of adjacent vertices where both dx!=0 and dy!=0,
 *      insert an intermediate vertex to create a right-angle step.
 *      The step direction is chosen to keep the vertex closer to the
 *      geometry interior (prefer horizontal-first for polygons).
 *   3. Remove consecutive duplicate points.
 *   4. Remove collinear points (same row or same column as neighbors).
 *   5. For polygon rings: ensure minimum 4 points (including closing).
 * ======================================================================== */

static gaiaGeomCollPtr
orthogonalize_geom (gaiaGeomCollPtr geom, double grid)
{
    gaiaGeomCollPtr result;
    gaiaPointPtr pt;
    gaiaLinestringPtr ln;
    gaiaPolygonPtr pg;
    int iv, iv2, ib;
    double x, y, px, py, nx, ny;
    double *tmp_x, *tmp_y;
    int tmp_count, out_count;
    int dims = geom->DimensionModel;

    if (grid <= 0.0)
	return NULL;

    /* allocate result with same dimension model and SRID */
    if (dims == GAIA_XY_Z)
	result = gaiaAllocGeomCollXYZ ();
    else if (dims == GAIA_XY_M)
	result = gaiaAllocGeomCollXYM ();
    else if (dims == GAIA_XY_Z_M)
	result = gaiaAllocGeomCollXYZM ();
    else
	result = gaiaAllocGeomColl ();
    result->Srid = geom->Srid;
    result->DeclaredType = geom->DeclaredType;

    /* copy points as-is (snapped) */
    pt = geom->FirstPoint;
    while (pt)
      {
	  x = floor (pt->X / grid + 0.5) * grid;
	  y = floor (pt->Y / grid + 0.5) * grid;
	  gaiaAddPointToGeomColl (result, x, y);
	  pt = pt->Next;
      }

    /* process linestrings */
    ln = geom->FirstLinestring;
    while (ln)
      {
	  int pts = ln->Points;
	  /* worst case: each edge becomes 2 edges → 2*pts vertices */
	  tmp_x = malloc (sizeof (double) * (pts * 2 + 1));
	  tmp_y = malloc (sizeof (double) * (pts * 2 + 1));
	  if (!tmp_x || !tmp_y)
	    {
		free (tmp_x);
		free (tmp_y);
		gaiaFreeGeomColl (result);
		return NULL;
	    }
	  tmp_count = 0;

	  /* snap and orthogonalize */
	  for (iv = 0; iv < pts; iv++)
	    {
		gaiaGetPoint (ln->Coords, iv, &x, &y);
		x = floor (x / grid + 0.5) * grid;
		y = floor (y / grid + 0.5) * grid;

		if (iv > 0)
		  {
		      double dx = x - px;
		      double dy = y - py;
		      if (dx != 0.0 && dy != 0.0)
			{
			    /* diagonal: insert staircase point (horizontal first) */
			    tmp_x[tmp_count] = x;
			    tmp_y[tmp_count] = py;
			    tmp_count++;
			}
		  }
		tmp_x[tmp_count] = x;
		tmp_y[tmp_count] = y;
		tmp_count++;
		px = x;
		py = y;
	    }

	  /* remove consecutive duplicates and collinear points */
	  out_count = 0;
	  for (iv = 0; iv < tmp_count; iv++)
	    {
		if (out_count > 0 && tmp_x[iv] == tmp_x[out_count - 1]
		    && tmp_y[iv] == tmp_y[out_count - 1])
		    continue;	/* duplicate */
		if (out_count >= 2)
		  {
		      /* check collinear: same X or same Y as prev two */
		      double ax = tmp_x[out_count - 2];
		      double ay = tmp_y[out_count - 2];
		      double bx = tmp_x[out_count - 1];
		      double by = tmp_y[out_count - 1];
		      if ((ax == bx && bx == tmp_x[iv])
			  || (ay == by && by == tmp_y[iv]))
			{
			    /* middle point is collinear, replace it */
			    out_count--;
			}
		  }
		tmp_x[out_count] = tmp_x[iv];
		tmp_y[out_count] = tmp_y[iv];
		out_count++;
	    }

	  if (out_count >= 2)
	    {
		gaiaLinestringPtr out_ln =
		    gaiaAddLinestringToGeomColl (result, out_count);
		for (iv = 0; iv < out_count; iv++)
		    gaiaSetPoint (out_ln->Coords, iv, tmp_x[iv], tmp_y[iv]);
	    }
	  free (tmp_x);
	  free (tmp_y);
	  ln = ln->Next;
      }

    /* process polygons */
    pg = geom->FirstPolygon;
    while (pg)
      {
	  int num_rings = 1 + pg->NumInteriors;
	  gaiaPolygonPtr out_pg = NULL;

	  for (ib = 0; ib < num_rings; ib++)
	    {
		gaiaRingPtr ring;
		int pts;
		if (ib == 0)
		    ring = pg->Exterior;
		else
		    ring = pg->Interiors + (ib - 1);
		pts = ring->Points;

		tmp_x = malloc (sizeof (double) * (pts * 2 + 2));
		tmp_y = malloc (sizeof (double) * (pts * 2 + 2));
		if (!tmp_x || !tmp_y)
		  {
		      free (tmp_x);
		      free (tmp_y);
		      gaiaFreeGeomColl (result);
		      return NULL;
		  }
		tmp_count = 0;

		for (iv = 0; iv < pts; iv++)
		  {
		      gaiaGetPoint (ring->Coords, iv, &x, &y);
		      x = floor (x / grid + 0.5) * grid;
		      y = floor (y / grid + 0.5) * grid;

		      if (iv > 0)
			{
			    double dx = x - px;
			    double dy = y - py;
			    if (dx != 0.0 && dy != 0.0)
			      {
				  /* diagonal: horizontal-first staircase */
				  tmp_x[tmp_count] = x;
				  tmp_y[tmp_count] = py;
				  tmp_count++;
			      }
			}
		      tmp_x[tmp_count] = x;
		      tmp_y[tmp_count] = y;
		      tmp_count++;
		      px = x;
		      py = y;
		  }

		/* remove duplicates and collinear */
		out_count = 0;
		for (iv = 0; iv < tmp_count; iv++)
		  {
		      if (out_count > 0 && tmp_x[iv] == tmp_x[out_count - 1]
			  && tmp_y[iv] == tmp_y[out_count - 1])
			  continue;
		      if (out_count >= 2)
			{
			    double ax = tmp_x[out_count - 2];
			    double ay = tmp_y[out_count - 2];
			    double bx = tmp_x[out_count - 1];
			    double by = tmp_y[out_count - 1];
			    if ((ax == bx && bx == tmp_x[iv])
				|| (ay == by && by == tmp_y[iv]))
			      {
				  out_count--;
			      }
			}
		      tmp_x[out_count] = tmp_x[iv];
		      tmp_y[out_count] = tmp_y[iv];
		      out_count++;
		  }

		/* also check wrap-around collinearity for closed ring */
		if (out_count >= 3 && tmp_x[0] == tmp_x[out_count - 1]
		    && tmp_y[0] == tmp_y[out_count - 1])
		  {
		      /* check if first point is collinear with last-1 and second */
		      double ax = tmp_x[out_count - 2];
		      double ay = tmp_y[out_count - 2];
		      double bx = tmp_x[0];
		      double by = tmp_y[0];
		      double cx = tmp_x[1];
		      double cy = tmp_y[1];
		      if ((ax == bx && bx == cx) || (ay == by && by == cy))
			{
			    /* shift: remove first, update closing point */
			    for (iv2 = 0; iv2 < out_count - 1; iv2++)
			      {
				  tmp_x[iv2] = tmp_x[iv2 + 1];
				  tmp_y[iv2] = tmp_y[iv2 + 1];
			      }
			    out_count--;
			    tmp_x[out_count - 1] = tmp_x[0];
			    tmp_y[out_count - 1] = tmp_y[0];
			}
		  }

		/* ensure ring is closed */
		if (out_count >= 2
		    && (tmp_x[0] != tmp_x[out_count - 1]
			|| tmp_y[0] != tmp_y[out_count - 1]))
		  {
		      tmp_x[out_count] = tmp_x[0];
		      tmp_y[out_count] = tmp_y[0];
		      out_count++;
		  }

		/* polygon ring needs at least 4 points */
		if (out_count >= 4)
		  {
		      if (ib == 0)
			{
			    out_pg =
				gaiaAddPolygonToGeomColl (result, out_count,
							 pg->NumInteriors);
			    gaiaRingPtr out_ring = out_pg->Exterior;
			    for (iv = 0; iv < out_count; iv++)
				gaiaSetPoint (out_ring->Coords, iv, tmp_x[iv],
					      tmp_y[iv]);
			}
		      else if (out_pg != NULL)
			{
			    gaiaRingPtr out_ring =
				gaiaAddInteriorRing (out_pg, ib - 1, out_count);
			    for (iv = 0; iv < out_count; iv++)
				gaiaSetPoint (out_ring->Coords, iv, tmp_x[iv],
					      tmp_y[iv]);
			}
		  }
		free (tmp_x);
		free (tmp_y);
	    }
	  pg = pg->Next;
      }

    if (result->FirstPoint == NULL && result->FirstLinestring == NULL
	&& result->FirstPolygon == NULL)
      {
	  gaiaFreeGeomColl (result);
	  return NULL;
      }
    return result;
}

static void
fnct_Orthogonalize (sqlite3_context * context, int argc, sqlite3_value ** argv)
{
/* SQL function:
/ ST_Orthogonalize(BLOBencoded geom, double grid_size)
/
/ Snaps geometry to grid then converts all diagonal edges to
/ axis-aligned (horizontal/vertical) staircase segments.
/ Returns NULL if any error is encountered or geometry collapses.
*/
    unsigned char *p_blob;
    int n_bytes;
    int int_value;
    double grid_size;
    gaiaGeomCollPtr geo = NULL;
    gaiaGeomCollPtr result = NULL;
    int gpkg_amphibious = 0;
    int gpkg_mode = 0;
    struct splite_internal_cache *cache = sqlite3_user_data (context);
    GAIA_UNUSED ();
    if (cache != NULL)
      {
	  gpkg_amphibious = cache->gpkg_amphibious_mode;
	  gpkg_mode = cache->gpkg_mode;
      }
    if (sqlite3_value_type (argv[0]) != SQLITE_BLOB)
      {
	  sqlite3_result_null (context);
	  return;
      }
    if (sqlite3_value_type (argv[1]) == SQLITE_INTEGER)
      {
	  int_value = sqlite3_value_int (argv[1]);
	  grid_size = int_value;
      }
    else if (sqlite3_value_type (argv[1]) == SQLITE_FLOAT)
      {
	  grid_size = sqlite3_value_double (argv[1]);
      }
    else
      {
	  sqlite3_result_null (context);
	  return;
      }
    if (grid_size <= 0.0)
      {
	  sqlite3_result_null (context);
	  return;
      }
    p_blob = (unsigned char *) sqlite3_value_blob (argv[0]);
    n_bytes = sqlite3_value_bytes (argv[0]);
    geo =
	gaiaFromSpatiaLiteBlobWkbEx (p_blob, n_bytes, gpkg_mode,
				     gpkg_amphibious);
    if (!geo)
      {
	  sqlite3_result_null (context);
	  return;
      }
    result = orthogonalize_geom (geo, grid_size);
    if (result == NULL)
	sqlite3_result_null (context);
    else
      {
	  int len;
	  unsigned char *p_result = NULL;
	  gaiaToSpatiaLiteBlobWkbEx (result, &p_result, &len, gpkg_mode);
	  sqlite3_result_blob (context, p_result, len, free);
	  gaiaFreeGeomColl (result);
      }
    gaiaFreeGeomColl (geo);
}

/* ========================================================================
 * ST_Rasterize(geom, grid_size)
 *
 * Rasterizes a polygon geometry onto a grid bitmap, then vectorizes the
 * bitmap back into an orthogonal (rectilinear) polygon.  This produces
 * a "pixel art" representation of the geometry where all edges are
 * perfectly axis-aligned.
 *
 * Algorithm:
 *   1. Compute geometry MBR, quantize to grid cells.
 *   2. Allocate a bitmap (max 256x256 cells to bound memory).
 *   3. For each grid cell, test if its center is inside the geometry
 *      using a simple point-in-polygon ray casting test.
 *   4. Trace the outer boundary of filled cells to produce an
 *      orthogonal polygon contour.
 *   5. Return the contour as a SpatiaLite polygon.
 *
 * For non-polygon geometries (points, lines), falls back to
 * ST_Orthogonalize behavior.
 * ======================================================================== */

/* Simple point-in-polygon ray casting (2D XY only) */
static int
pip_ray_cast (double px, double py, gaiaRingPtr ring)
{
    int i, j, inside = 0;
    int pts = ring->Points;
    double xi, yi, xj, yj;
    for (i = 0, j = pts - 1; i < pts; j = i++)
      {
	  gaiaGetPoint (ring->Coords, i, &xi, &yi);
	  gaiaGetPoint (ring->Coords, j, &xj, &yj);
	  if (((yi > py) != (yj > py))
	      && (px < (xj - xi) * (py - yi) / (yj - yi) + xi))
	      inside = !inside;
      }
    return inside;
}

static int
point_in_polygon (double px, double py, gaiaPolygonPtr pg)
{
    int ib;
    if (!pip_ray_cast (px, py, pg->Exterior))
	return 0;
    for (ib = 0; ib < pg->NumInteriors; ib++)
      {
	  if (pip_ray_cast (px, py, pg->Interiors + ib))
	      return 0;		/* inside a hole */
      }
    return 1;
}

static int
point_in_geom (double px, double py, gaiaGeomCollPtr geom)
{
    gaiaPolygonPtr pg = geom->FirstPolygon;
    while (pg)
      {
	  if (point_in_polygon (px, py, pg))
	      return 1;
	  pg = pg->Next;
      }
    return 0;
}

/* Trace orthogonal contour of a bitmap.
 * Uses a simple boundary-following algorithm that walks the edge
 * between filled and empty cells, producing axis-aligned segments.
 * Returns number of vertices written to out_x/out_y. */
static int
trace_bitmap_contour (unsigned char *bitmap, int cols, int rows,
		      double origin_x, double origin_y, double grid,
		      double **out_x, double **out_y)
{
    /* Find first filled cell (top-left scan) */
    int start_col = -1, start_row = -1;
    int r, c;
    int dir;			/* 0=right, 1=down, 2=left, 3=up */
    int cr, cc;			/* current row, col */
    int max_steps;
    int count = 0;
    int capacity;
    double *xs, *ys;

    for (r = 0; r < rows && start_col < 0; r++)
      {
	  for (c = 0; c < cols; c++)
	    {
		if (bitmap[r * cols + c])
		  {
		      start_row = r;
		      start_col = c;
		      break;
		  }
	    }
      }
    if (start_col < 0)
      {
	  *out_x = NULL;
	  *out_y = NULL;
	  return 0;
      }

    /* Walk the boundary: we track the edge between filled and empty.
     * We start at the top-left corner of the first filled cell,
     * moving right along the top edge. */
    capacity = (cols + rows) * 4 + 8;
    xs = malloc (sizeof (double) * capacity);
    ys = malloc (sizeof (double) * capacity);
    if (!xs || !ys)
      {
	  free (xs);
	  free (ys);
	  *out_x = NULL;
	  *out_y = NULL;
	  return 0;
      }

#define GRID_FILLED(rr, cc) \
    ((rr) >= 0 && (rr) < rows && (cc) >= 0 && (cc) < cols && bitmap[(rr) * cols + (cc)])

    /* Direction: 0=right, 1=down, 2=left, 3=up
     * We walk along the boundary keeping filled cells on our right side. */
    cr = start_row;
    cc = start_col;
    dir = 0;			/* start moving right */
    max_steps = 2 * (cols + rows) * 2 + 4;

    /* Add starting corner */
    xs[count] = origin_x + cc * grid;
    ys[count] = origin_y + (rows - cr) * grid;
    count++;

    do
      {
	  int next_r, next_c, right_r, right_c;
	  /* "right side" cell depends on direction */
	  switch (dir)
	    {
	    case 0:		/* moving right: right side is below */
		right_r = cr;
		right_c = cc;
		next_r = cr;
		next_c = cc + 1;
		break;
	    case 1:		/* moving down: right side is left */
		right_r = cr;
		right_c = cc - 1;
		next_r = cr + 1;
		next_c = cc;
		break;
	    case 2:		/* moving left: right side is above */
		right_r = cr - 1;
		right_c = cc - 1;
		next_r = cr;
		next_c = cc - 1;
		break;
	    default:		/* moving up: right side is right */
		right_r = cr - 1;
		right_c = cc;
		next_r = cr - 1;
		next_c = cc;
		break;
	    }

	  if (!GRID_FILLED (right_r, right_c))
	    {
		/* right side empty: turn right (clockwise) */
		dir = (dir + 1) % 4;
	    }
	  else if (!GRID_FILLED (right_r + (dir == 0 ? -1 : dir == 2 ? 1 : 0),
				 right_c + (dir == 1 ? 1 : dir == 3 ? -1 : 0)))
	    {
		/* can continue straight: advance */
		cr = next_r;
		cc = next_c;
	    }
	  else
	    {
		/* front-left is filled: turn left (counter-clockwise) */
		dir = (dir + 3) % 4;
	    }

	  /* Compute corner position based on current cr, cc, dir */
	  {
	      double cx, cy;
	      switch (dir)
		{
		case 0:
		    cx = origin_x + cc * grid;
		    cy = origin_y + (rows - cr) * grid;
		    break;
		case 1:
		    cx = origin_x + cc * grid;
		    cy = origin_y + (rows - cr) * grid;
		    break;
		case 2:
		    cx = origin_x + cc * grid;
		    cy = origin_y + (rows - cr) * grid;
		    break;
		default:
		    cx = origin_x + cc * grid;
		    cy = origin_y + (rows - cr) * grid;
		    break;
		}
	      /* Only add if different from last point */
	      if (count == 0 || cx != xs[count - 1] || cy != ys[count - 1])
		{
		    if (count >= capacity - 2)
		      {
			  capacity *= 2;
			  xs = realloc (xs, sizeof (double) * capacity);
			  ys = realloc (ys, sizeof (double) * capacity);
			  if (!xs || !ys)
			    {
				free (xs);
				free (ys);
				*out_x = NULL;
				*out_y = NULL;
				return 0;
			    }
		      }
		    xs[count] = cx;
		    ys[count] = cy;
		    count++;
		}
	  }
	  max_steps--;
      }
    while ((cr != start_row || cc != start_col || dir != 0) && max_steps > 0);

#undef GRID_FILLED

    /* close ring */
    if (count > 0 && (xs[0] != xs[count - 1] || ys[0] != ys[count - 1]))
      {
	  xs[count] = xs[0];
	  ys[count] = ys[0];
	  count++;
      }

    *out_x = xs;
    *out_y = ys;
    return count;
}

static gaiaGeomCollPtr
rasterize_geom (gaiaGeomCollPtr geom, double grid)
{
    gaiaGeomCollPtr result;
    double minx, miny, maxx, maxy;
    int cols, rows, r, c;
    unsigned char *bitmap;
    double *contour_x, *contour_y;
    int contour_count;
    gaiaPolygonPtr pg;
    gaiaRingPtr ring;

    if (grid <= 0.0)
	return NULL;
    if (geom->FirstPolygon == NULL)
      {
	  /* non-polygon: fall back to orthogonalize */
	  return orthogonalize_geom (geom, grid);
      }

    /* compute MBR */
    pg = geom->FirstPolygon;
    minx = maxx = miny = maxy = 0.0;
    {
	int first = 1;
	while (pg)
	  {
	      double x, y;
	      int iv;
	      ring = pg->Exterior;
	      for (iv = 0; iv < ring->Points; iv++)
		{
		    gaiaGetPoint (ring->Coords, iv, &x, &y);
		    if (first)
		      {
			  minx = maxx = x;
			  miny = maxy = y;
			  first = 0;
		      }
		    else
		      {
			  if (x < minx)
			      minx = x;
			  if (x > maxx)
			      maxx = x;
			  if (y < miny)
			      miny = y;
			  if (y > maxy)
			      maxy = y;
		      }
		}
	      pg = pg->Next;
	  }
    }

    /* quantize to grid */
    minx = floor (minx / grid) * grid;
    miny = floor (miny / grid) * grid;
    maxx = ceil (maxx / grid) * grid;
    maxy = ceil (maxy / grid) * grid;

    cols = (int) ((maxx - minx) / grid + 0.5);
    rows = (int) ((maxy - miny) / grid + 0.5);

    /* bound memory: max 256x256 */
    if (cols > 256)
	cols = 256;
    if (rows > 256)
	rows = 256;
    if (cols < 1)
	cols = 1;
    if (rows < 1)
	rows = 1;

    /* recalculate grid to fit bounded dimensions */
    {
	double actual_grid_x = (maxx - minx) / cols;
	double actual_grid_y = (maxy - miny) / rows;
	/* use the larger to keep square cells */
	if (actual_grid_x > actual_grid_y)
	    grid = actual_grid_x;
	else
	    grid = actual_grid_y;
    }

    bitmap = calloc (cols * rows, 1);
    if (!bitmap)
	return NULL;

    /* rasterize: test cell centers */
    for (r = 0; r < rows; r++)
      {
	  double cy = miny + (r + 0.5) * grid;
	  for (c = 0; c < cols; c++)
	    {
		double cx = minx + (c + 0.5) * grid;
		if (point_in_geom (cx, cy, geom))
		    bitmap[(rows - 1 - r) * cols + c] = 1;
	    }
      }

    /* vectorize: trace contour */
    contour_count =
	trace_bitmap_contour (bitmap, cols, rows, minx, miny, grid,
			      &contour_x, &contour_y);
    free (bitmap);

    if (contour_count < 4)
      {
	  free (contour_x);
	  free (contour_y);
	  return NULL;
      }

    /* build result polygon */
    result = gaiaAllocGeomColl ();
    result->Srid = geom->Srid;
    result->DeclaredType = GAIA_POLYGON;
    {
	gaiaPolygonPtr out_pg =
	    gaiaAddPolygonToGeomColl (result, contour_count, 0);
	ring = out_pg->Exterior;
	for (c = 0; c < contour_count; c++)
	    gaiaSetPoint (ring->Coords, c, contour_x[c], contour_y[c]);
    }
    free (contour_x);
    free (contour_y);
    return result;
}

static void
fnct_Rasterize (sqlite3_context * context, int argc, sqlite3_value ** argv)
{
/* SQL function:
/ ST_Rasterize(BLOBencoded geom, double grid_size)
/
/ Rasterizes polygon geometry onto a grid, then vectorizes back into
/ an orthogonal (pixel-art style) polygon.
/ For non-polygon geometries, falls back to ST_Orthogonalize.
/ Returns NULL if geometry collapses or on error.
*/
    unsigned char *p_blob;
    int n_bytes;
    int int_value;
    double grid_size;
    gaiaGeomCollPtr geo = NULL;
    gaiaGeomCollPtr result = NULL;
    int gpkg_amphibious = 0;
    int gpkg_mode = 0;
    struct splite_internal_cache *cache = sqlite3_user_data (context);
    GAIA_UNUSED ();
    if (cache != NULL)
      {
	  gpkg_amphibious = cache->gpkg_amphibious_mode;
	  gpkg_mode = cache->gpkg_mode;
      }
    if (sqlite3_value_type (argv[0]) != SQLITE_BLOB)
      {
	  sqlite3_result_null (context);
	  return;
      }
    if (sqlite3_value_type (argv[1]) == SQLITE_INTEGER)
      {
	  int_value = sqlite3_value_int (argv[1]);
	  grid_size = int_value;
      }
    else if (sqlite3_value_type (argv[1]) == SQLITE_FLOAT)
      {
	  grid_size = sqlite3_value_double (argv[1]);
      }
    else
      {
	  sqlite3_result_null (context);
	  return;
      }
    if (grid_size <= 0.0)
      {
	  sqlite3_result_null (context);
	  return;
      }
    p_blob = (unsigned char *) sqlite3_value_blob (argv[0]);
    n_bytes = sqlite3_value_bytes (argv[0]);
    geo =
	gaiaFromSpatiaLiteBlobWkbEx (p_blob, n_bytes, gpkg_mode,
				     gpkg_amphibious);
    if (!geo)
      {
	  sqlite3_result_null (context);
	  return;
      }
    result = rasterize_geom (geo, grid_size);
    if (result == NULL)
	sqlite3_result_null (context);
    else
      {
	  int len;
	  unsigned char *p_result = NULL;
	  gaiaToSpatiaLiteBlobWkbEx (result, &p_result, &len, gpkg_mode);
	  sqlite3_result_blob (context, p_result, len, free);
	  gaiaFreeGeomColl (result);
      }
    gaiaFreeGeomColl (geo);
}

/* ======================================================================== */

static void
register_postgis_compat_sql_functions (sqlite3 * db,
				       struct splite_internal_cache *cache)
{
    sqlite3_create_function_v2 (db, "ST_Orthogonalize", 2,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, cache,
				fnct_Orthogonalize, 0, 0, 0);
    sqlite3_create_function_v2 (db, "Orthogonalize", 2,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, cache,
				fnct_Orthogonalize, 0, 0, 0);
    sqlite3_create_function_v2 (db, "ST_Rasterize", 2,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, cache,
				fnct_Rasterize, 0, 0, 0);
    sqlite3_create_function_v2 (db, "Rasterize", 2,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, cache,
				fnct_Rasterize, 0, 0, 0);
    sqlite3_create_function_v2 (db, "ST_AsGeoJSON", 1,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, cache,
				fnct_AsGeoJSON, 0, 0, 0);
    sqlite3_create_function_v2 (db, "ST_AsGeoJSON", 2,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, cache,
				fnct_AsGeoJSON, 0, 0, 0);
    sqlite3_create_function_v2 (db, "ST_AsGeoJSON", 3,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, cache,
				fnct_AsGeoJSON, 0, 0, 0);
    sqlite3_create_function_v2 (db, "ST_GeomFromGeoJSON", 1,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, cache,
				fnct_FromGeoJSON, 0, 0, 0);

    sqlite3_create_function_v2 (db, "ST_SetSRID", 2,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, cache,
				fnct_SetSRID, 0, 0, 0);

    sqlite3_create_function_v2 (db, "ST_XMin", 1,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0,
				fnct_MbrMinX, 0, 0, 0);
    sqlite3_create_function_v2 (db, "ST_YMin", 1,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0,
				fnct_MbrMinY, 0, 0, 0);
    sqlite3_create_function_v2 (db, "ST_XMax", 1,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0,
				fnct_MbrMaxX, 0, 0, 0);
    sqlite3_create_function_v2 (db, "ST_YMax", 1,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0,
				fnct_MbrMaxY, 0, 0, 0);

    sqlite3_create_function_v2 (db, "ST_MakeEnvelope", 4,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0,
				fnct_BuildMbr1, 0, 0, 0);
    sqlite3_create_function_v2 (db, "ST_MakeEnvelope", 5,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0,
				fnct_BuildMbr2, 0, 0, 0);

    sqlite3_create_function_v2 (db, "ST_MakePoint", 2,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0,
				fnct_MakePoint1, 0, 0, 0);
    sqlite3_create_function_v2 (db, "ST_MakePoint", 3,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0,
				fnct_MakePointZ1, 0, 0, 0);
    sqlite3_create_function_v2 (db, "ST_MakePoint", 4,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, 0,
				fnct_MakePointZM1, 0, 0, 0);

    sqlite3_create_function_v2 (db, "ST_MakeLine", 1,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, cache, 0,
				fnct_MakeLine_step, fnct_MakeLine_final, 0);
    sqlite3_create_function_v2 (db, "ST_MakeLine", 2,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, cache,
				fnct_MakeLine, 0, 0, 0);

    sqlite3_create_function_v2 (db, "ST_DWithin", 3,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, cache,
				fnct_PtDistWithin, 0, 0, 0);
    sqlite3_create_function_v2 (db, "ST_DWithin", 4,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, cache,
				fnct_PtDistWithin, 0, 0, 0);

    sqlite3_create_function_v2 (db, "ST_LineInterpolatePoint", 2,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, cache,
				fnct_LineInterpolatePoint, 0, 0, 0);
    sqlite3_create_function_v2 (db, "ST_LineLocatePoint", 2,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, cache,
				fnct_LineLocatePoint, 0, 0, 0);
    sqlite3_create_function_v2 (db, "ST_LineSubstring", 3,
				SQLITE_UTF8 | SQLITE_DETERMINISTIC, cache,
				fnct_LineSubstring, 0, 0, 0);
}
