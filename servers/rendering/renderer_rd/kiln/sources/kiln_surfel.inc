// lighting restir-meets-surfel surfel pool, clipmap grid and lookup, ported for the
// parity comparison. Sources: lighting assets/shaders/surfel_gi/{surfel_constants,
// surfel_grid_hash,surfel_binning_shared,lookup}.hlsl. The half-migrated hash table
// of the reference is reduced to its effective modulo behavior.
//
// Buffer binding contract shared by every KILN_* stage (set 0), kept clear of
// the bvh.inc and kiln surfel bindings:
//  40 surfel_meta_buf       8 x u32    [1] live count, [2] alloc pointer
//  41 surfel_pool_buf       u32 x MAX  LIFO free-index stack
//  42 cell_index_offset_buf u32 x MAX+1 per-cell inclusive prefix sum
//  43 surfel_index_buf      u32 x MAX*24 sorted surfel ids per cell
//  44 surfel_spatial_buf    vec4 x MAX xyz = position, w bits = packed normal 11:10:11
//  45 surfel_irradiance_buf vec4 x MAX rgb = irradiance, a = accumulated samples
//  46 surfel_aux_buf        vec4 x MAX*2 [2i] blended radiance + count, [2i] MSME state
//  47 surfel_life_buf       u32 x MAX  age; valid < 100
//  48 surfel_reposition_proposal_buf vec4 x MAX next trace origin proposal

#define KILN_MAX_SURFEL_CELLS 262144u
#define KILN_MAX_SURFELS 262144u
#define KILN_MAX_SURFELS_PER_CELL 128u
#define KILN_MAX_SURFELS_PER_CELL_FOR_KEEP_ALIVE 32u
#define KILN_SURFELS_PER_CELL_SLOTS 24u
#define KILN_SURFEL_CS 32u

const float KILN_SURFEL_GRID_CELL_DIAMETER = 0.2;
const float KILN_SURFEL_BASE_RADIUS = 0.24;
const float KILN_SURFEL_RADIUS_OVERSCALE = 1.25; // [sic] reference typo preserved
const float KILN_SURFEL_NORMAL_DIRECTION_SQUISH = 2.0;

const uint KILN_SURFEL_META_SURFEL_COUNT = 4u;
const uint KILN_SURFEL_META_ALLOC_COUNT = 8u;
const uint KILN_SURFEL_LIFE_RECYCLE = 0x8000000u;
const uint KILN_SURFEL_LIFE_RECYCLED = KILN_SURFEL_LIFE_RECYCLE + 1u;

bool kiln_is_surfel_life_valid(uint life) {
	return life < 100u;
}

bool kiln_surfel_life_needs_aging(uint life) {
	return life != KILN_SURFEL_LIFE_RECYCLED;
}

// Access qualifiers reflect each stage's use into RD dependencies, following the
// kiln sources convention. The meta buffer is read-write everywhere: the count
// is read by most stages while allocation and aging issue atomics on it.
#define KILN_META_ACCESS
#ifdef STAGE_KILN_SURFEL_CLEAR_POOL
#define KILN_POOL_ACCESS writeonly
#elif defined(STAGE_KILN_SURFEL_ALLOCATE) || defined(STAGE_KILN_SURFEL_AGE)
#define KILN_POOL_ACCESS
#else
#define KILN_POOL_ACCESS readonly
#endif
#if defined(STAGE_KILN_SURFEL_CLEAR_CELLS) || defined(STAGE_KILN_SURFEL_COUNT_CELLS) || defined(STAGE_KILN_SURFEL_SLOT_CELLS)
#define KILN_CELL_ACCESS
#else
#define KILN_CELL_ACCESS readonly
#endif
#ifdef STAGE_KILN_SURFEL_SLOT_CELLS
#define KILN_INDEX_ACCESS
#else
#define KILN_INDEX_ACCESS readonly
#endif
#if defined(STAGE_KILN_SURFEL_ALLOCATE)
#define KILN_SPATIAL_ACCESS
#else
#define KILN_SPATIAL_ACCESS readonly
#endif
#if defined(STAGE_KILN_SURFEL_ALLOCATE) || defined(STAGE_KILN_SURFEL_TRACE)
#define KILN_IRRADIANCE_ACCESS
#else
#define KILN_IRRADIANCE_ACCESS readonly
#endif
#if defined(STAGE_KILN_SURFEL_TRACE) || defined(STAGE_KILN_SURFEL_ALLOCATE)
#define KILN_AUX_ACCESS
#else
#define KILN_AUX_ACCESS readonly
#endif
#define KILN_LIFE_ACCESS
#if defined(STAGE_KILN_SURFEL_FIND_MISSING) || defined(STAGE_KILN_SURFEL_ALLOCATE)
#define KILN_PROPOSAL_ACCESS
#else
#define KILN_PROPOSAL_ACCESS readonly
#endif

layout(set = 0, binding = 40, std430) KILN_META_ACCESS buffer KilnMetaBuf { uint kiln_surfel_meta_buf[]; };
layout(set = 0, binding = 41, std430) KILN_POOL_ACCESS buffer KilnPoolBuf { uint kiln_surfel_pool_buf[]; };
layout(set = 0, binding = 42, std430) KILN_CELL_ACCESS buffer KilnCellOffsetBuf { uint kiln_cell_index_offset_buf[]; };
layout(set = 0, binding = 43, std430) KILN_INDEX_ACCESS buffer KilnIndexBuf { uint kiln_surfel_index_buf[]; };
layout(set = 0, binding = 44, std430) KILN_SPATIAL_ACCESS buffer KilnSpatialBuf { vec4 kiln_surfel_spatial_buf[]; };
layout(set = 0, binding = 45, std430) KILN_IRRADIANCE_ACCESS buffer KilnIrradianceBuf { vec4 kiln_surfel_irradiance_buf[]; };
layout(set = 0, binding = 46, std430) KILN_AUX_ACCESS buffer KilnAuxBuf { vec4 kiln_surfel_aux_buf[]; };
layout(set = 0, binding = 47, std430) KILN_LIFE_ACCESS buffer KilnLifeBuf { uint kiln_surfel_life_buf[]; };
layout(set = 0, binding = 48, std430) KILN_PROPOSAL_ACCESS buffer KilnProposalBuf { vec4 kiln_surfel_reposition_proposal_buf[]; };

vec3 kiln_surfel_position(uint idx) {
	return kiln_surfel_spatial_buf[idx].xyz;
}

vec3 kiln_surfel_normal(uint idx) {
	return kiln_unpack_normal_11_10_11_no_normalize(floatBitsToUint(kiln_surfel_spatial_buf[idx].w));
}

// ---- surfel_grid_hash.hlsl ----
vec3 kiln_get_eye_position() {
	return p.inv_view[3].xyz;
}

vec3 kiln_get_prev_eye_position() {
	return p.kiln_prev_eye.xyz;
}

ivec3 kiln_surfel_pos_to_grid_coord(vec3 pos, vec3 eye_pos) {
	return ivec3(floor((pos - eye_pos) / KILN_SURFEL_GRID_CELL_DIAMETER));
}

vec3 kiln_surfel_grid_coord_center(uvec4 coord, vec3 eye_pos) {
	return eye_pos + ((vec3(coord.xyz) + 0.5 - float(KILN_SURFEL_CS) * 0.5) * KILN_SURFEL_GRID_CELL_DIAMETER) * float(1u << coord.w);
}

float kiln_surfel_grid_coord_to_cascade_float(ivec3 coord) {
	const vec3 fcoord = vec3(coord) + 0.5;
	const float max_coord = max(abs(fcoord.x), max(abs(fcoord.y), abs(fcoord.z)));
	return log2(max_coord / (float(KILN_SURFEL_CS) * 0.5));
}

uint kiln_cascade_float_to_cascade(float cascade_float) {
	return uint(clamp(ceil(max(0.0, cascade_float)), 0.0, 7.0));
}

uint kiln_surfel_grid_coord_to_cascade(ivec3 coord) {
	return kiln_cascade_float_to_cascade(kiln_surfel_grid_coord_to_cascade_float(coord));
}

float kiln_surfel_radius_for_pos(vec3 pos) {
	return KILN_SURFEL_BASE_RADIUS * max(1.0, length(pos - kiln_get_eye_position()) / (KILN_SURFEL_GRID_CELL_DIAMETER * float(KILN_SURFEL_CS) * 0.5));
}

ivec3 kiln_surfel_grid_coord_within_cascade(ivec3 coord, uint cascade) {
	return (coord >> int(cascade)) + int(KILN_SURFEL_CS / 2);
}

uvec4 kiln_surfel_grid_coord_to_c4(ivec3 coord) {
	const uint cascade = kiln_surfel_grid_coord_to_cascade(coord);
	const uvec3 ucoord_in_cascade = uvec3(clamp(kiln_surfel_grid_coord_within_cascade(coord, cascade), ivec3(0), ivec3(KILN_SURFEL_CS - 1u)));
	return uvec4(ucoord_in_cascade, cascade);
}

uint kiln_surfel_grid_c4_to_hash(uvec4 c4) {
	// GLSL has no integer dot(); the reference relies on HLSL's uint4 dot.
	return c4.x + c4.y * KILN_SURFEL_CS + c4.z * (KILN_SURFEL_CS * KILN_SURFEL_CS) + c4.w * (KILN_SURFEL_CS * KILN_SURFEL_CS * KILN_SURFEL_CS);
}

uint kiln_surfel_grid_coord_to_hash(ivec3 coord) {
	return kiln_surfel_grid_c4_to_hash(kiln_surfel_grid_coord_to_c4(coord)) % KILN_MAX_SURFEL_CELLS;
}

uint kiln_cell_range_begin(uint cell_idx) {
	return kiln_cell_index_offset_buf[cell_idx];
}

uint kiln_cell_range_end(uint cell_idx) {
	return kiln_cell_index_offset_buf[cell_idx + 1u];
}

// ---- surfel_binning_shared.hlsl ----
uint kiln_surfel_cascade_for_pos(vec3 position) {
	const float fc = kiln_surfel_grid_coord_to_cascade_float(kiln_surfel_pos_to_grid_coord(position, kiln_get_eye_position()));
	return kiln_cascade_float_to_cascade(fc);
}

bool kiln_surfel_intersects_grid_coord(vec3 position, vec3 normal, uvec4 grid_coord) {
	const float surfel_radius = kiln_surfel_radius_for_pos(position);

	const vec3 cell_center = kiln_surfel_grid_coord_center(grid_coord, kiln_get_eye_position());
	const float grid_cell_radius = (KILN_SURFEL_GRID_CELL_DIAMETER * 0.5) * float(1u << grid_coord.w);

	const vec3 cell_local_surfel_pos = position - cell_center;
	const vec3 cell_local_closest_point_on_grid_cell = clamp(cell_local_surfel_pos, vec3(-grid_cell_radius), vec3(grid_cell_radius));

	const vec3 pos_offset = cell_local_surfel_pos - cell_local_closest_point_on_grid_cell;

	// Approximate box-ellipsoid culling. Sometimes misses corners, but greatly
	// improves culling efficiency (reference preserves this behavior).
	const float mahalanobis_dist = length(pos_offset) * (1.0 + abs(dot(pos_offset, normal)) * KILN_SURFEL_NORMAL_DIRECTION_SQUISH);
	return mahalanobis_dist < surfel_radius;
}

struct KilnSurfelGridMinMax {
	uvec4 c4_min[2];
	uvec4 c4_max[2];
	uint cascade_count;
};

KilnSurfelGridMinMax kiln_get_surfel_grid_box_min_max(vec3 position) {
	const float surfel_radius = kiln_surfel_radius_for_pos(position);

	const vec3 box_min_pos = position - surfel_radius;
	const vec3 box_max_pos = position + surfel_radius;

	const vec3 eye_pos = kiln_get_eye_position();
	const float fc = kiln_surfel_grid_coord_to_cascade_float(kiln_surfel_pos_to_grid_coord(position, eye_pos));

	const uint c0 = kiln_cascade_float_to_cascade(fc - 0.2);
	const uint c1 = kiln_cascade_float_to_cascade(fc + 0.2);

	const ivec3 min_coord = kiln_surfel_pos_to_grid_coord(box_min_pos, eye_pos);
	const ivec3 max_coord = kiln_surfel_pos_to_grid_coord(box_max_pos, eye_pos);

	KilnSurfelGridMinMax result;

	result.cascade_count = 1u;
	result.c4_min[0] = uvec4(clamp(kiln_surfel_grid_coord_within_cascade(min_coord, c0), ivec3(0), ivec3(KILN_SURFEL_CS - 1u)), c0);
	result.c4_max[0] = uvec4(clamp(kiln_surfel_grid_coord_within_cascade(max_coord, c0), ivec3(0), ivec3(KILN_SURFEL_CS - 1u)), c0);

	if (c1 != c0) {
		result.cascade_count = 2u;
		result.c4_min[1] = uvec4(clamp(kiln_surfel_grid_coord_within_cascade(min_coord, c1), ivec3(0), ivec3(KILN_SURFEL_CS - 1u)), c1);
		result.c4_max[1] = uvec4(clamp(kiln_surfel_grid_coord_within_cascade(max_coord, c1), ivec3(0), ivec3(KILN_SURFEL_CS - 1u)), c1);
	}

	return result;
}

// The reference updates irradiance in place during the trace dispatch. Keep
// that transport schedule; independent runs can differ with GPU work ordering.
// ---- lookup.hlsl ----
vec3 kiln_lookup_surfel_gi(vec3 pt_ws, vec3 normal_ws) {
	const uint cell_idx = kiln_surfel_grid_coord_to_hash(kiln_surfel_pos_to_grid_coord(pt_ws, kiln_get_eye_position()));

	uint range_begin = kiln_cell_range_begin(cell_idx);
	uint range_end = kiln_cell_range_end(cell_idx);
	// TEMP HACK (reference): never iterate over tons of surfels out of bounds.
	range_end = min(range_end, range_begin + KILN_MAX_SURFELS_PER_CELL);

	vec3 total_color = vec3(0.0);
	float total_weight = 0.0;

	for (uint surfel_idx_loc = range_begin; surfel_idx_loc < range_end; ++surfel_idx_loc) {
		const uint surfel_idx = kiln_surfel_index_buf[surfel_idx_loc];

#ifndef KILN_SURFEL_LOOKUP_DONT_KEEP_ALIVE
		if (range_end - range_begin <= KILN_MAX_SURFELS_PER_CELL_FOR_KEEP_ALIVE) {
			kiln_surfel_life_buf[surfel_idx] = 0u;
		}
#endif

		const vec3 surfel_position = kiln_surfel_position(surfel_idx);
		const vec3 surfel_normal = kiln_surfel_normal(surfel_idx);
        const vec4 surfel_irradiance_packed = kiln_surfel_irradiance_buf[surfel_idx];

		const vec3 pos_offset = pt_ws - surfel_position;
		const float directional_weight = max(0.0, dot(surfel_normal, normal_ws));
		const float mahalanobis_dist = length(pos_offset) * (1.0 + abs(dot(pos_offset, surfel_normal)) * KILN_SURFEL_NORMAL_DIRECTION_SQUISH);

		const float surfel_radius = kiln_surfel_radius_for_pos(surfel_position);
		const float weight = (1.0 - smoothstep(0.0, surfel_radius * KILN_SURFEL_RADIUS_OVERSCALE, mahalanobis_dist)) * directional_weight;

		total_weight += weight;
		total_color += surfel_irradiance_packed.xyz * weight;
	}

	total_color /= max(0.1, total_weight);
	return total_color;
}
