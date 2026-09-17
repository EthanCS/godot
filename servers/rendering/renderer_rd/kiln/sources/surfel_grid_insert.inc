	vec4 sphere = surfels[id].position_radius;
	uint level = surfel_level(sphere.w);
	float spacing = cell_size(level);
	ivec3 lo = ivec3(floor((sphere.xyz - sphere.w) / spacing));
	ivec3 hi = ivec3(floor((sphere.xyz + sphere.w) / spacing));
	uint keys[27];
	uint count = 0u;
	for (int z = lo.z; z <= hi.z; z++) {
		for (int y = lo.y; y <= hi.y; y++) {
			for (int x = lo.x; x <= hi.x; x++) {
				vec3 cell_min = vec3(x, y, z) * spacing;
				vec3 closest = clamp(sphere.xyz, cell_min, cell_min + spacing);
				vec3 delta = closest - sphere.xyz;
				if (dot(delta, delta) >= sphere.w * sphere.w) continue;
				// The cache has thin plane support; reject cells which cannot
				// intersect that slab, as well as cells outside the support sphere.
				vec3 normal = surfels[id].normal_age.xyz;
				float extent = dot(abs(normal), vec3(spacing * 0.5));
				float plane = abs(dot(cell_min + spacing * 0.5 - sphere.xyz, normal));
				if (plane > extent + max(0.012, sphere.w * 0.06)) continue;
				uint key = cell_hash(ivec3(x, y, z), level);
				// A hash collision among this surfel's own cells must not insert it
				// twice into the same bucket (which would double its contribution).
				bool duplicate = false;
				for (uint i = 0u; i < count; i++) duplicate = duplicate || keys[i] == key;
				if (duplicate) continue;
				keys[count++] = key;
				uint entry = atomicAdd(cell_heads[key].x, 1u);
#ifdef STAGE_SURFEL_GRID_SCATTER
				cell_links[cell_heads[key].y + grid_sums[key / 64u] + entry] = id;
#endif
			}
		}
	}
