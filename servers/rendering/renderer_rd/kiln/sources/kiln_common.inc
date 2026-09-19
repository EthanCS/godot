// lighting restir-meets-surfel shared math, ported for the parity comparison.
// Sources: lighting assets/shaders/inc/{hash,quasi_random,reservoir,working_color_space,
// pack_unpack}.hlsl and atmosphere_felix.hlsl (Felix Westin, MIT). The RNG streams are
// reproduced bit-for-bit so per-frame noise patterns line up between the engines.

const float KILN_M_PLASTIC = 1.32471795724474602596;
const float KILN_M_CBRT_2 = 1.2599210498948731647672106072782283505;
const float KILN_INFINITY = 1.0 / 0.0;

// ---- hash.hlsl ----
uint kiln_hash1(uint x) {
	x += (x << 10u);
	x ^= (x >> 6u);
	x += (x << 3u);
	x ^= (x >> 11u);
	x += (x << 15u);
	return x;
}

uint kiln_hash1_mut(inout uint h) {
	uint res = h;
	h = kiln_hash1(h);
	return res;
}

uint kiln_hash_combine2(uint x, uint y) {
	const uint M = 1664525u, C = 1013904223u;
	uint seed = (x * M + y + C) * M;
	// Tempering (from Matsumoto)
	seed ^= (seed >> 11u);
	seed ^= (seed << 7u) & 0x9d2c5680u;
	seed ^= (seed << 15u) & 0xefc60000u;
	seed ^= (seed >> 18u);
	return seed;
}

uint kiln_hash2(uvec2 v) {
	return kiln_hash_combine2(v.x, kiln_hash1(v.y));
}

uint kiln_hash3(uvec3 v) {
	return kiln_hash_combine2(v.x, kiln_hash2(v.yz));
}

uint kiln_hash4(uvec4 v) {
	return kiln_hash_combine2(v.x, kiln_hash3(v.yzw));
}

float kiln_uint_to_u01_float(uint h) {
	const uint mantissa_mask = 0x007FFFFFu;
	const uint one = 0x3F800000u;
	h &= mantissa_mask;
	h |= one;
	return uintBitsToFloat(h) - 1.0;
}

float kiln_interleaved_gradient_noise(uvec2 px) {
	return fract(52.9829189 * fract(0.06711056 * float(px.x) + 0.00583715 * float(px.y)));
}

// ---- quasi_random.hlsl ----
float kiln_radical_inverse_vdc(uint bits) {
	bits = (bits << 16u) | (bits >> 16u);
	bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
	bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
	bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
	bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
	return float(bits) * 2.3283064365386963e-10; // / 0x100000000
}

vec2 kiln_hammersley(uint i, uint n) {
	return vec2(float(i + 1) / n, kiln_radical_inverse_vdc(i + 1));
}

vec2 kiln_r2_sequence(uint i) {
	const float a1 = 1.0 / KILN_M_PLASTIC;
	const float a2 = 1.0 / (KILN_M_PLASTIC * KILN_M_PLASTIC);
	return fract(vec2(a1, a2) * i + 0.5);
}

// ---- reservoir.hlsl ----
struct KilnReservoir1spp {
	float w_sum;
	uint payload;
	float M;
	float W;
};

KilnReservoir1spp kiln_reservoir_create() {
	KilnReservoir1spp res;
	res.w_sum = 0.0;
	res.payload = 0u;
	res.M = 0.0;
	res.W = 0.0;
	return res;
}

KilnReservoir1spp kiln_reservoir_from_raw(vec4 raw) {
	KilnReservoir1spp res;
	res.w_sum = raw.x;
	res.payload = floatBitsToUint(raw.y);
	res.M = raw.z;
	res.W = raw.w;
	return res;
}

vec4 kiln_reservoir_as_raw(KilnReservoir1spp res) {
	return vec4(res.w_sum, uintBitsToFloat(res.payload), res.M, res.W);
}

bool kiln_reservoir_update(inout KilnReservoir1spp res, float w, uint sample_payload, inout uint rng) {
	res.w_sum += w;
	res.M += 1.0;
	const float dart = kiln_uint_to_u01_float(kiln_hash1_mut(rng));
	const float prob = w / res.w_sum;
	if (prob >= dart) {
		res.payload = sample_payload;
		return true;
	}
	return false;
}

// ---- working_color_space.hlsl (crunched luma-chroma used by temporal_filter2) ----
vec3 kiln_rgb_to_ycbcr(vec3 col) {
	return transpose(mat3(0.2126, 0.7152, 0.0722, -0.1146, -0.3854, 0.5, 0.5, -0.4542, -0.0458)) * col;
}

vec3 kiln_ycbcr_to_rgb(vec3 col) {
	return max(vec3(0.0), transpose(mat3(1.0, 0.0, 1.5748, 1.0, -0.1873, -0.4681, 1.0, 1.8556, 0.0)) * col);
}

vec4 kiln_linear_rgb_to_crunched_luma_chroma(vec4 v) {
	v.rgb = kiln_rgb_to_ycbcr(v.rgb);
	float k = sqrt(v.x) / max(1e-8, v.x);
	return vec4(v.rgb * k, v.a);
}

vec4 kiln_crunched_luma_chroma_to_linear_rgb(vec4 v) {
	v.rgb *= v.x;
	v.rgb = kiln_ycbcr_to_rgb(v.rgb);
	return v;
}

// ---- pack_unpack.hlsl ----
float kiln_unpack_unorm(uint pckd, uint bit_count) {
	uint max_val = (1u << bit_count) - 1u;
	return float(pckd & max_val) / float(max_val);
}

uint kiln_pack_unorm(float val, uint bit_count) {
	uint max_val = (1u << bit_count) - 1u;
	return uint(clamp(val, 0.0, 1.0) * float(max_val));
}

float kiln_pack_normal_11_10_11(vec3 n) {
	uint pckd = 0u;
	pckd += kiln_pack_unorm(n.x * 0.5 + 0.5, 11u);
	pckd += kiln_pack_unorm(n.y * 0.5 + 0.5, 10u) << 11u;
	pckd += kiln_pack_unorm(n.z * 0.5 + 0.5, 11u) << 21u;
	return uintBitsToFloat(pckd);
}

vec3 kiln_unpack_normal_11_10_11(uint p) {
	return normalize(vec3(
		kiln_unpack_unorm(p, 11u),
		kiln_unpack_unorm(p >> 11u, 10u),
		kiln_unpack_unorm(p >> 21u, 11u)) * 2.0 - 1.0);
}

vec3 kiln_unpack_normal_11_10_11_no_normalize(uint p) {
	return vec3(
		kiln_unpack_unorm(p, 11u),
		kiln_unpack_unorm(p >> 11u, 10u),
		kiln_unpack_unorm(p >> 21u, 11u)) * 2.0 - 1.0;
}

// ---- atmosphere_felix.hlsl (Felix Westin, MIT; lighting USE_FELIX_ATMOSPHERE path) ----
const float KILN_PLANET_RADIUS = 6371000.0;
const vec3 KILN_PLANET_CENTER = vec3(0.0, -KILN_PLANET_RADIUS, 0.0);
const float KILN_ATMOSPHERE_HEIGHT = 100000.0;
const float KILN_RAYLEIGH_HEIGHT = KILN_ATMOSPHERE_HEIGHT * 0.08;
const float KILN_MIE_HEIGHT = KILN_ATMOSPHERE_HEIGHT * 0.012;
const vec3 KILN_C_RAYLEIGH = vec3(5.802, 13.558, 33.100) * 1e-6;
const vec3 KILN_C_MIE = vec3(3.996, 3.996, 3.996) * 1e-6;
const vec3 KILN_C_OZONE = vec3(0.650, 1.881, 0.085) * 1e-6;
const float KILN_ATMOSPHERE_DENSITY = 1.0;
const float KILN_ATMOSPHERE_EXPOSURE = 20.0;

vec2 kiln_sphere_intersection(vec3 ray_start, vec3 ray_dir, vec3 sphere_center, float sphere_radius) {
	ray_start -= sphere_center;
	float a = dot(ray_dir, ray_dir);
	float b = 2.0 * dot(ray_start, ray_dir);
	float c = dot(ray_start, ray_start) - (sphere_radius * sphere_radius);
	float d = b * b - 4.0 * a * c;
	if (d < 0.0) {
		return vec2(-1.0);
	}
	d = sqrt(d);
	return vec2(-b - d, -b + d) / (2.0 * a);
}

vec2 kiln_planet_intersection(vec3 ray_start, vec3 ray_dir) {
	return kiln_sphere_intersection(ray_start, ray_dir, KILN_PLANET_CENTER, KILN_PLANET_RADIUS);
}

vec2 kiln_atmosphere_intersection(vec3 ray_start, vec3 ray_dir) {
	return kiln_sphere_intersection(ray_start, ray_dir, KILN_PLANET_CENTER, KILN_PLANET_RADIUS + KILN_ATMOSPHERE_HEIGHT);
}

float kiln_phase_rayleigh(float costh) {
	return 3.0 * (1.0 + costh * costh) / (16.0 * PI);
}

float kiln_phase_mie(float costh, float g) {
	g = min(g, 0.9381);
	float k = 1.55 * g - 0.55 * g * g * g;
	float kcosth = k * costh;
	return (1.0 - k * k) / ((4.0 * PI) * (1.0 - kcosth) * (1.0 - kcosth));
}

float kiln_atmosphere_height(vec3 position_ws) {
	return distance(position_ws, KILN_PLANET_CENTER) - KILN_PLANET_RADIUS;
}

float kiln_density_rayleigh(float h) {
	return exp(-max(0.0, h / KILN_RAYLEIGH_HEIGHT));
}

float kiln_density_mie(float h) {
	return exp(-max(0.0, h / KILN_MIE_HEIGHT));
}

float kiln_density_ozone(float h) {
	// Tent function, 30 km wide, centered at 25 km altitude.
	return max(0.0, 1.0 - abs(h - 25000.0) / 15000.0);
}

vec3 kiln_atmosphere_density(float h) {
	return vec3(kiln_density_rayleigh(h), kiln_density_mie(h), kiln_density_ozone(h));
}

vec3 kiln_integrate_optical_depth(vec3 ray_start, vec3 ray_dir) {
	vec2 intersection = kiln_atmosphere_intersection(ray_start, ray_dir);
	float ray_length = intersection.y;

	const int sample_count = 8;
	float step_size = ray_length / float(sample_count);

	vec3 optical_depth = vec3(0.0);
	for (int i = 0; i < sample_count; i++) {
		vec3 local_position = ray_start + ray_dir * (float(i) + 0.5) * step_size;
		float local_height = kiln_atmosphere_height(local_position);
		vec3 local_density = kiln_atmosphere_density(local_height);
		optical_depth += local_density * step_size;
	}
	return optical_depth;
}

vec3 kiln_absorb(vec3 optical_depth) {
	// Mie results in slightly more absorption than scattering, about 10%.
	return exp(-(optical_depth.x * KILN_C_RAYLEIGH + optical_depth.y * KILN_C_MIE * 1.1 + optical_depth.z * KILN_C_OZONE) * KILN_ATMOSPHERE_DENSITY);
}

vec3 kiln_integrate_scattering(vec3 ray_start, vec3 ray_dir, float ray_length, vec3 light_dir, vec3 light_color, out vec3 transmittance) {
	float sample_distribution_exponent = 5.0;

	vec2 intersection = kiln_atmosphere_intersection(ray_start, ray_dir);
	ray_length = min(ray_length, intersection.y);
	if (intersection.x > 0.0) {
		// Advance ray to the atmosphere entry point.
		ray_start += ray_dir * intersection.x;
		ray_length -= intersection.x;
	}

	float costh = dot(ray_dir, light_dir);
	float phase_r = kiln_phase_rayleigh(costh);
	float phase_m = kiln_phase_mie(costh, 0.85);

	const int sample_count = 16;

	vec3 optical_depth = vec3(0.0);
	vec3 rayleigh = vec3(0.0);
	vec3 mie = vec3(0.0);

	float prev_ray_time = 0.0;

	for (int i = 1; i <= sample_count; i++) {
		float ray_time = pow(float(i) / float(sample_count), sample_distribution_exponent) * ray_length;
		float step_size = (ray_time - prev_ray_time);

		vec3 local_position = ray_start + ray_dir * mix(prev_ray_time, ray_time, 0.5);
		float local_height = kiln_atmosphere_height(local_position);
		vec3 local_density = kiln_atmosphere_density(local_height);

		optical_depth += local_density * step_size;

		// The atmospheric transmittance from ray_start to local_position.
		vec3 view_transmittance = kiln_absorb(optical_depth);

		vec3 optical_depth_light = kiln_integrate_optical_depth(local_position, light_dir);
		// The atmospheric transmittance of light reaching local_position.
		vec3 light_transmittance = kiln_absorb(optical_depth_light);

		rayleigh += view_transmittance * light_transmittance * phase_r * local_density.x * step_size;
		mie += view_transmittance * light_transmittance * phase_m * local_density.y * step_size;

		prev_ray_time = ray_time;
	}

	transmittance = kiln_absorb(optical_depth);

	return (rayleigh * KILN_C_RAYLEIGH + mie * KILN_C_MIE) * light_color * KILN_ATMOSPHERE_EXPOSURE;
}

// ---- sun.hlsl / atmosphere.hlsl (lighting's sun and sky for the GI path) ----
// lighting evaluates the atmosphere from the world origin for both the sun color and
// the sky cube; scene content is assumed to sit within a few km of the origin.
vec3 kiln_sun_color(vec3 sun_direction, vec3 sun_color_multiplier) {
	return 20.0 * sun_color_multiplier * kiln_absorb(kiln_integrate_optical_depth(vec3(0.0), sun_direction));
}

vec3 kiln_atmosphere_default(vec3 wi, vec3 light_dir, vec3 sky_ambient, vec3 sun_color_multiplier) {
	vec3 ray_start = vec3(0.0);
	vec3 ray_dir = wi;
	float ray_length = KILN_INFINITY;

	vec3 transmittance;
	return sky_ambient + sun_color_multiplier *
			kiln_integrate_scattering(ray_start, ray_dir, ray_length, light_dir, vec3(1.0), transmittance);
}

// Kajiya inc/math.hlsl, Frisvad orthonormal frame (GLSL column vectors).
mat3 kiln_orthonormal_basis(vec3 n) {
    if (n.z < 0.0) {
        float a = 1.0 / (1.0 - n.z), b = n.x * n.y * a;
        return mat3(vec3(1.0 - n.x*n.x*a, -b, n.x), vec3(b, n.y*n.y*a - 1.0, -n.y), n);
    }
    float a = 1.0 / (1.0 + n.z), b = -n.x*n.y*a;
    return mat3(vec3(1.0 - n.x*n.x*a, b, -n.x), vec3(b, 1.0 - n.y*n.y*a, -n.y), n);
}
