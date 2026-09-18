// Handwritten Metal implementation of Kiln's GGX reflection pass.
// Engine licensing: LICENSE.txt. Surfel cache adaptation attribution:
// ../licenses/SURFELPLUS-NOTICE.txt. This file is embedded verbatim, not transpiled.
#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;
using namespace metal::raytracing;

constant uint reflection_ray_count [[function_constant(0)]];
constant float PI = 3.14159265358979323846f;
constant uint CELL_LINK_ID_MASK = 262143u;

struct Parameters {
	float4x4 projection, inv_projection, inv_view, view, previous_view_projection;
	float4 size_frame, gi, quality, voxel_min, voxel_size, voxel_state;
	float4 sun_direction, sun_color, sky_color, debug, indirect_tint, sky_high;
	float4 fake_light_color, fake_light_direction, fake_light2_color, fake_light2_direction;
	float4 ground_escape, source_bvh_state, dynamic_scene;
	float4x4 previous_inverse_view_projection;
	float4 engine_state;
};
static_assert(sizeof(Parameters) == KILN_PARAMETER_BYTES, "Kiln parameter ABI mismatch");
struct Triangle { float4 p0, p1, p2, albedo, emission; };
struct TextureCoordinates { float4 ab, c_page_repeat; };
struct Surfel {
	float4 position_radius, normal_age, irradiance_samples;
	float4 short_mean_vbbr, variance_inconsistency;
	uint4 anchor;
	float4 barycentric, local_normal;
};
static_assert(sizeof(Triangle) == 80 && sizeof(Surfel) == 128 && sizeof(TextureCoordinates) == 32, "Kiln storage ABI mismatch");

static inline float3 unit(float3 v) { return v * rsqrt(max(dot(v, v), 1e-16f)); }
static inline float luminance(float3 v) { return dot(v, float3(0.2126f, 0.7152f, 0.0722f)); }
static inline uint hash(uint x) {
	x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu;
	return x ^ (x >> 16);
}
static inline float random_float(thread uint &seed) {
	seed = hash(seed + 0x9e3779b9u);
	return float(seed >> 8) * (1.0f / 16777216.0f);
}
static inline float2 random_pair(thread uint &seed) {
	float x = random_float(seed);
	return float2(x, random_float(seed));
}
static inline float3 cosine_direction(float3 n, float2 xi) {
	float r = sqrt(xi.x), phi = 2.0f * PI * xi.y;
	float3 tangent = unit(cross(abs(n.y) < 0.95f ? float3(0, 1, 0) : float3(1, 0, 0), n));
	return tangent * (r * cos(phi)) + cross(n, tangent) * (r * sin(phi)) + n * sqrt(max(0.0f, 1.0f - xi.x));
}
// Same procedural sky as kiln/demo/rendering/sky/sky_radiance.gdshaderinc.
static inline float cloud_union(float a, float b) {
	float h = max(0.26f - abs(a - b), 0.0f) / 0.26f;
	return min(a, b) - h * h * 0.065f;
}
static inline float clouds(float3 direction, float coverage) {
	if (coverage <= 0.0f || direction.y < -0.12f || direction.y > 0.55f) return 0.0f;
	float cloud = 0.0f;
	for (int i = 0; i < 12; ++i) {
		float seed = fract(float(i) * 0.618033989f + 0.31f), angle = float(i) * 2.39996323f;
		float2 facing(cos(angle), sin(angle));
		if (dot(direction.xz, facing) < 0.86f) continue;
		float width = mix(0.09f, 0.19f, seed) * mix(0.45f, 1.4f, coverage);
		float height = mix(0.022f, 0.055f, seed);
		float2 uv(dot(direction.xz, float2(-facing.y, facing.x)) / width,
				(direction.y - mix(0.035f, 0.22f, fract(seed * 3.7f))) / height);
		float d = length(uv / float2(1.1f, 0.40f)) - 1.0f;
		d = cloud_union(d, length((uv - float2(-0.55f, 0.25f)) / float2(0.47f, 0.60f)) - 1.0f);
		d = cloud_union(d, length((uv - float2(0.0f, 0.48f)) / float2(0.55f, 0.90f)) - 1.0f);
		d = cloud_union(d, length((uv - float2(0.58f, 0.20f)) / float2(0.42f, 0.55f)) - 1.0f);
		d += sin(uv.x * 13.0f + sin(uv.y * 9.0f)) * sin(uv.y * 11.0f) * 0.025f;
		cloud = max(cloud, (1.0f - smoothstep(-0.14f, 0.16f, d)) * smoothstep(0.0f, 0.15f, coverage));
	}
	return cloud * smoothstep(-0.12f, 0.015f, direction.y);
}
static inline float3 project_sky(float3 direction, constant Parameters &p) {
	float elevation = pow(clamp(1.0f - pow(1.0f - clamp(direction.y + 0.2f, 0.0f, 1.0f), 14.0f), 0.0f, 1.0f), 0.65f);
	float time = p.fake_light_direction.w;
	float night = 1.0f - (smoothstep(0.483f, 0.505f, time) - smoothstep(0.84f, 0.91f, time)) * 0.75f;
	float3 base = mix(p.sky_color.rgb * 1.5f, p.sky_high.rgb, elevation) * night;
	float angle = acos(clamp(dot(p.sun_direction.xyz, direction), -1.0f, 1.0f));
	float distance = max(0.0f, angle - 0.0261799395f);
	float a = 0.5f + 25.0f * distance, b = 1.0f + 5.0f * distance;
	float3 halo = float3(1, 0.65f, 0.2f) * (5.0f / (a * a)) + float3(1, 0.75f, 0.6f) * (0.8f / (b * b));
	float cloud = clouds(direction, p.fake_light2_color.w);
	float cloud_light = 0.70f + 0.30f * smoothstep(-0.05f, 0.28f, direction.y);
	base = mix(base, p.fake_light2_color.rgb * cloud_light * night, cloud);
	float3 color = base + halo * p.fake_light_color.w * (1.0f - cloud * 0.85f);
	return max(float3(0), mix(float3(luminance(color)), color, p.fake_light2_direction.x));
}
static inline float3 environment_radiance(float3 direction, constant Parameters &p) {
	if (p.sky_high.w < 0.5f) return p.sky_color.rgb * p.sun_color.w;
	if (direction.y <= 0.0f) return p.ground_escape.rgb * p.sun_color.w;
	if (p.sky_high.w > 1.5f) return project_sky(direction, p) * p.sun_color.w;
	float alignment = dot(direction, p.fake_light_direction.xyz);
	float3 directional_tint = mix(mix(float3(0.2f, 0.4f, 1), float3(1, 0.4f, 0.2f), alignment * 0.5f + 0.5f), float3(1), direction.y * direction.y);
	float3 fake = (p.fake_light_color.rgb * pow(max(0.0f, alignment), 12.0f)
			+ p.fake_light2_color.rgb * pow(max(0.0f, dot(direction, p.fake_light2_direction.xyz)), 12.0f)) * 3.0f;
	float3 tint = max(float3(0), float3(1) - p.indirect_tint.rgb * 0.9200000166893005f);
	float sky_mix = pow(clamp(1.0f - pow(1.0f - clamp(direction.y + 0.2f, 0.0f, 1.0f), 14.0f), 0.0f, 1.0f), 0.6499999761581421f);
	float3 sky = mix(p.sky_color.rgb, p.sky_high.rgb, sky_mix) * 1.0999999046325684f * directional_tint / max(luminance(directional_tint), 1e-6f);
	sky += max(float3(0), mix(tint / max(luminance(tint), 1e-6f) * luminance(fake), fake, 0.8547008633613586f) * 0.8333333134651184f);
	return sky * p.sun_color.w;
}
// GGX visible-normal sampling: Heitz, JCGT 7(4), 2018.
static inline float3 visible_microfacet(float3 view, float alpha, float2 xi) {
	float3 stretched = unit(float3(alpha * view.xy, view.z));
	float3 tangent = stretched.z < 0.9999f ? unit(cross(float3(0, 0, 1), stretched)) : float3(1, 0, 0);
	float angle = 2.0f * PI * xi.y;
	float2 disk = sqrt(xi.x) * float2(cos(angle), sin(angle));
	float blend = 0.5f * (1.0f + stretched.z);
	disk.y = mix(sqrt(max(0.0f, 1.0f - disk.x * disk.x)), disk.y, blend);
	float3 hemisphere = tangent * disk.x + cross(stretched, tangent) * disk.y + stretched * sqrt(max(0.0f, 1.0f - dot(disk, disk)));
	return unit(float3(alpha * hemisphere.xy, max(0.0f, hemisphere.z)));
}
static inline float smith_lambda(float cosine, float alpha) {
	return 0.5f * (sqrt(1.0f + alpha * alpha * max(0.0f, 1.0f - cosine * cosine) / max(cosine * cosine, 1e-8f)) - 1.0f);
}

struct Sample { float4 base, fresnel; };
struct Scene {
	constant Parameters &p;
	texture2d<float> depth, normals;
	device const Triangle *triangles, *dynamic_triangles;
	device const float4 *emitters;
	device const Surfel *surfels;
	device const uint2 *cell_heads;
	device const uint *cell_links;
	device const float4 *local_lights;
	device const uint *light_grid;
	instance_acceleration_structure tlas;
	texture2d<uint> surface;
	texture2d_array<float> albedo;
	sampler albedo_sampler;
	device const TextureCoordinates *static_uvs, *dynamic_uvs;
	device const uint *grid_sums;

	Triangle triangle(uint id) const {
		uint count = uint(p.source_bvh_state.y);
		return id < count ? triangles[id] : dynamic_triangles[id - count];
	}
	float3 hit_albedo(uint id, float3 position, thread const Triangle &t) const {
		uint count = uint(p.source_bvh_state.y);
		TextureCoordinates uv_record = id < count ? static_uvs[id] : dynamic_uvs[id - count];
		if (uv_record.c_page_repeat.z < 0.5f) return t.albedo.rgb;
		float3 delta = position - t.p0.xyz;
		float aa = dot(t.p1.xyz, t.p1.xyz), ab = dot(t.p1.xyz, t.p2.xyz), bb = dot(t.p2.xyz, t.p2.xyz);
		float da = dot(delta, t.p1.xyz), db = dot(delta, t.p2.xyz);
		float2 bary = float2(da * bb - db * ab, db * aa - da * ab) / max(aa * bb - ab * ab, 1e-20f);
		float2 uv = uv_record.ab.xy * (1.0f - bary.x - bary.y) + uv_record.ab.zw * bary.x + uv_record.c_page_repeat.xy * bary.y;
		uv = uv_record.c_page_repeat.w > 0.5f ? fract(uv) : clamp(uv, float2(0.5f / 512.0f), float2(511.5f / 512.0f));
		return t.albedo.rgb * albedo.sample(albedo_sampler, uv, uint(uv_record.c_page_repeat.z - 1.0f), level(0.0f)).rgb;
	}
	bool blocked(float3 origin, float3 direction, float maximum = 1000.0f) const {
		intersector<instancing> tracer;
		tracer.assume_geometry_type(geometry_type::triangle);
		tracer.force_opacity(forced_opacity::opaque);
		tracer.accept_any_intersection(true);
		return tracer.intersect(ray(origin, direction, 0.006f, maximum), tlas, 255u).type != intersection_type::none;
	}
	float4 gather(float3 position, float3 normal) const {
		float3 sum(0); float weight = 0.0f;
		uint candidates = 0u;
		for (uint level = 0; level < 3; ++level) {
			if (candidates >= 32u) break;
			int3 c = int3(floor(position / (0.25f * float(1u << level))));
			uint hash_value = hash(uint(c.x) * 73856093u ^ uint(c.y) * 19349663u ^ uint(c.z) * 83492791u ^ level * 2654435761u);
			uint key = hash_value & CELL_LINK_ID_MASK;
			uint2 cell = cell_heads[key];
			uint offset = cell.y + grid_sums[key / 64u];
			for (uint i = 0; i < cell.x; ++i) {
				if (candidates >= 32u) break;
				uint link = cell_links[offset + i];
				if (((link ^ hash_value) & ~CELL_LINK_ID_MASK) != 0u) continue;
				uint id = link & CELL_LINK_ID_MASK;
				float4 sphere = surfels[id].position_radius;
				float3 delta = position - sphere.xyz;
				float squared = dot(delta, delta);
				uint surfel_level = sphere.w <= 0.25f ? 0u : (sphere.w <= 0.5f ? 1u : 2u);
				if (surfel_level != level || squared >= sphere.w * sphere.w) continue;
				float3 sn = surfels[id].normal_age.xyz;
				float alignment = dot(normal, sn);
				if (alignment < 0.85f || abs(dot(delta, sn)) > max(0.012f, sphere.w * 0.06f)) continue;
				float w = (1.0f - sqrt(squared) / sphere.w) * max(0.0f, alignment);
				w = w * w * (3.0f - 2.0f * w);
				float4 irradiance = surfels[id].irradiance_samples;
				float confidence = smoothstep(0.0f, 64.0f, irradiance.w);
				if (confidence <= 1e-4f) continue;
				// Only a compatible, admitted lighting contributor consumes the cap.
				candidates++;
				sum += irradiance.rgb * w * confidence;
				weight += w * confidence;
			}
		}
		return float4(weight > 1e-6f ? sum / weight : float3(0), weight);
	}
	float3 local_unoccluded(uint id, float3 position, float3 normal, thread float3 &direction, thread float &distance) const {
		float4 pr = local_lights[2u + id * 4u], color = local_lights[3u + id * 4u], cone = local_lights[4u + id * 4u];
		float3 delta = pr.xyz - position;
		distance = length(delta); direction = delta / max(distance, 0.0001f);
		if (distance >= pr.w || distance < 0.001f) return float3(0);
		float nl = max(0.0f, dot(normal, direction));
		if (nl <= 0.0f) return float3(0);
		float window = max(1.0f - pow(distance / pr.w, 4.0f), 0.0f);
		float attenuation = window * window * pow(max(distance, 0.0001f), -color.w);
		if (cone.w >= -1.0f) {
			float scos = max(dot(-direction, cone.xyz), cone.w);
			float rim = max(1e-4f, (1.0f - scos) / (1.0f - cone.w));
			attenuation *= 1.0f - pow(rim, local_lights[5u + id * 4u].x);
		}
		return max(float3(0), color.rgb * (attenuation * nl / PI));
	}
	float3 local_sample(float3 position, float3 normal, float xi) const {
		float4 origin = local_lights[0]; int3 dims = int3(local_lights[1].xyz);
		int3 cell = int3(floor((position - origin.xyz) / origin.w));
		if (any(cell < int3(0)) || any(cell >= dims)) return float3(0);
		int cell_id = (cell.z * dims.y + cell.y) * dims.x + cell.x;
		uint count = light_grid[cell_id * 2 + 1], start = light_grid[cell_id * 2];
		float total = 0.0f, distance; float3 direction;
		for (uint i = 0; i < count; ++i) total += luminance(local_unoccluded(light_grid[start + i], position, normal, direction, distance));
		if (total <= 1e-10f) return float3(0);
		float target = xi * total, accum = 0.0f;
		for (uint i = 0; i < count; ++i) {
			float3 value = local_unoccluded(light_grid[start + i], position, normal, direction, distance);
			float weight = luminance(value); accum += weight;
			if (weight > 0.0f && (accum >= target || i + 1u == count)) {
				return blocked(position + normal * 0.025f, direction, distance - 0.03f) ? float3(0) : value * (total / weight);
			}
		}
		return float3(0);
	}
	float3 sky_sample(float3 position, float3 normal, float2 xi) const {
		float3 direction = cosine_direction(normal, xi);
		if (p.sun_color.w <= 0.0f || direction.y <= 0.0f) return float3(0);
		return blocked(position + normal * 0.025f, direction) ? float3(0) : environment_radiance(direction, p);
	}
	float3 emitter_sample(float3 position, float3 normal, float3 xi, thread float3 &direction) const {
		direction = normal;
		int count = int(p.dynamic_scene.z);
		if (count == 0) return float3(0);
		float target = xi.x * p.dynamic_scene.w; int lo = 0, hi = count - 1;
		while (lo < hi) { int mid = (lo + hi) / 2; if (emitters[mid].y < target) lo = mid + 1; else hi = mid; }
		float4 entry = emitters[lo]; Triangle tri = triangle(uint(entry.x));
		float u = sqrt(xi.y);
		float3 point = tri.p0.xyz + tri.p1.xyz * (u * (1.0f - xi.z)) + tri.p2.xyz * (u * xi.z);
		float3 origin = position + normal * 0.012f, delta = point - origin;
		float squared = dot(delta, delta);
		if (squared < 0.0004f) return float3(0);
		float distance = sqrt(squared); direction = delta / distance;
		if (dot(normal, direction) <= 0.0f) return float3(0);
		float cosine = abs(dot(unit(cross(tri.p1.xyz, tri.p2.xyz)), -direction));
		if (cosine < 0.0001f || blocked(origin, direction, distance - 0.008f)) return float3(0);
		return tri.emission.rgb * (entry.z * cosine * p.dynamic_scene.w / (squared * entry.w));
	}
	float3 reflected_radiance(float3 origin, float3 direction, thread uint &seed, thread float &distance) const {
		intersector<instancing> tracer;
		tracer.assume_geometry_type(geometry_type::triangle);
		tracer.force_opacity(forced_opacity::opaque);
		tracer.accept_any_intersection(false);
		auto hit = tracer.intersect(ray(origin, direction, 0.006f, 1000.0f), tlas, 255u);
		if (hit.type == intersection_type::none) {
			distance = 1000.0f;
			return environment_radiance(direction, p);
		}
		uint id = hit.primitive_id + (hit.user_instance_id == 1u ? uint(p.source_bvh_state.y) : 0u);
		distance = hit.distance;
		Triangle tri = triangle(id);
		float3 normal = unit(float3(tri.p0.w, tri.p1.w, tri.p2.w));
		if (dot(normal, direction) > 0.0f) normal = -normal;
		float3 position = origin + direction * distance, incident(0);
		float nl = max(dot(normal, p.sun_direction.xyz), 0.0f);
		if (nl > 0.0f && p.sun_direction.w > 0.0f && !blocked(position + normal * 0.025f, p.sun_direction.xyz)) {
			incident += p.sun_color.rgb * (p.sun_direction.w * nl / PI);
		}
		incident += local_sample(position, normal, random_float(seed));
		float4 cached = gather(position, normal);
		if (cached.a > 0.1f) incident += cached.rgb;
		else {
			incident += sky_sample(position, normal, random_pair(seed));
			float2 pair = random_pair(seed); float3 xi(pair, random_float(seed));
			float3 emitter_direction;
			float3 emitted = emitter_sample(position, normal, xi, emitter_direction);
			incident += emitted * max(dot(normal, emitter_direction), 0.0f) / PI;
		}
		return tri.emission.rgb + clamp(hit_albedo(id, position, tri), float3(0), float3(0.95f)) * incident;
	}
	float3 view_position(uint2 pixel, float d) const {
		float2 uv = (float2(pixel) + 0.5f) / p.size_frame.xy;
		float4 point = p.inv_projection * float4(uv * 2.0f - 1.0f, d, 1.0f);
		return point.xyz / point.w;
	}
	float3 shading_normal(float4 material) const {
		return unit(p.engine_state.x > 0.5f ? material.xyz : material.xyz * 2.0f - 1.0f);
	}
	float roughness(float4 material) const {
		return clamp(p.engine_state.x > 0.5f ? material.a : min(material.a, 1.0f - material.a) * (255.0f / 127.0f), 0.0f, 1.0f);
	}
	Sample sample(uint2 pixel, uint lane) const {
		Sample result = { float4(0), float4(0) };
		uint2 size = uint2(p.size_frame.xy);
		if (any(pixel >= size)) return result;
		float d = depth.read(pixel).r;
		if (d <= 1e-7f || p.source_bvh_state.z <= 0.5f || p.debug.w <= 0.5f) return result;
		float4 material = normals.read(pixel);
		float r = roughness(material);
		float3 view_normal = shading_normal(material), view_point = view_position(pixel, d);
		if (p.voxel_state.y > 0.5f && r >= 0.45f && ((pixel.x + pixel.y + uint(p.size_frame.z)) & 1u) != 0u) {
			uint2 neighbor(pixel.x ^ 1u, pixel.y);
			if (neighbor.x < size.x) {
				float nd = depth.read(neighbor).r;
				float4 nm = normals.read(neighbor); float nr = roughness(nm);
				if (nd > 1e-7f && nr >= 0.45f && abs(nr - r) <= 0.025f && dot(view_normal, shading_normal(nm)) >= 0.99f &&
						abs(dot(view_position(neighbor, nd) - view_point, view_normal)) <= 0.02f) {
					result.base.a = -1.0f;
					return result;
				}
			}
		}
		float3x3 rotation(p.inv_view[0].xyz, p.inv_view[1].xyz, p.inv_view[2].xyz);
		float3 position = (p.inv_view * float4(view_point, 1)).xyz;
		float3 n = unit(rotation * view_normal), geometric = n;
		if (p.engine_state.y > 0.5f) {
			uint packed = surface.read(pixel).g;
			float2 oct = max(float2(short(packed & 65535u), short(packed >> 16)) / 32767.0f, float2(-1));
			float3 gn(oct, 1.0f - abs(oct.x) - abs(oct.y));
			gn.xy += select(float2(1), float2(-1), gn.xy >= float2(0)) * max(-gn.z, 0.0f);
			geometric = unit(rotation * unit(gn));
		}
		float3 v = unit(p.inv_view[3].xyz - position);
		float3 tangent = unit(cross(abs(n.y) < 0.95f ? float3(0, 1, 0) : float3(1, 0, 0), n));
		float3x3 basis(tangent, cross(n, tangent), n);
		float3 local_view = transpose(basis) * v;
		float alpha = max(0.002f, r * r);
		uint base_seed = hash((pixel.x + pixel.y * size.x) ^ uint(p.size_frame.z) * 1664525u);
		float3 base(0), fresnel(0); float mean_distance = 0.0f;
		for (uint i = lane; i < reflection_ray_count; i += 2u) {
			uint seed = i == 0u ? base_seed : hash(base_seed ^ (i * 0x9e3779b9u));
			float3 h = basis * visible_microfacet(float3(local_view.xy, max(0.0001f, local_view.z)), alpha, random_pair(seed));
			float3 direction = reflect(-v, h);
			float nl = dot(n, direction), nv = max(dot(n, v), 0.0001f);
			if (nl <= 0.0f || dot(geometric, direction) <= 0.0f) continue;
			float lv = smith_lambda(nv, alpha);
			float weight = (1.0f + lv) / (1.0f + lv + smith_lambda(nl, alpha));
			float schlick = pow(1.0f - clamp(dot(v, h), 0.0f, 1.0f), 5.0f);
			float distance;
			float3 radiance = min(reflected_radiance(position + geometric * 0.025f, direction, seed, distance), float3(256));
			base += radiance * weight * (1.0f - schlick);
			fresnel += radiance * weight * schlick;
			mean_distance += distance;
		}
		return { float4(base, mean_distance) / float(reflection_ray_count), float4(fresnel / float(reflection_ray_count), 0) };
	}
};

#if KILN_ARGUMENT_BUFFERS
struct Resources {
	// RD writes Tier-2 resource IDs as a dense table. Keep unused bindings and
	// samplers as padding too: [[id]] alone does not insert byte-layout holes.
	constant Parameters *parameters [[id(KILN_BUFFER_0)]];
	texture2d<float> depth [[id(KILN_TEXTURE_1)]];
	sampler depth_sampler [[id(KILN_SAMPLER_1)]];
	texture2d<float> normals [[id(KILN_TEXTURE_2)]];
	sampler normal_sampler [[id(KILN_SAMPLER_2)]];
	device const float4 *nodes [[id(KILN_BUFFER_4)]];
	device const Triangle *triangles [[id(KILN_BUFFER_5)]];
	texture2d<float, access::write> base [[id(KILN_TEXTURE_6)]];
	texture2d<float, access::write> fresnel [[id(KILN_TEXTURE_7)]];
	device const float4 *dynamic_nodes [[id(KILN_BUFFER_16)]];
	device const Triangle *dynamic_triangles [[id(KILN_BUFFER_17)]];
	device const float4 *emitters [[id(KILN_BUFFER_18)]];
	device const Surfel *surfels [[id(KILN_BUFFER_20)]];
	device const uint2 *cell_heads [[id(KILN_BUFFER_21)]];
	device const uint *cell_links [[id(KILN_BUFFER_22)]];
	device const uint *free_slots [[id(KILN_BUFFER_23)]];
	device const uint *counters [[id(KILN_BUFFER_24)]];
	device const float4 *local_lights [[id(KILN_BUFFER_25)]];
	device const uint *light_grid [[id(KILN_BUFFER_26)]];
	instance_acceleration_structure tlas [[id(KILN_BUFFER_27)]];
	texture2d<uint> surface [[id(KILN_TEXTURE_31)]];
	sampler surface_sampler [[id(KILN_SAMPLER_31)]];
	texture2d_array<float> albedo [[id(KILN_TEXTURE_32)]];
	sampler albedo_sampler [[id(KILN_SAMPLER_32)]];
	device const TextureCoordinates *static_uvs [[id(KILN_BUFFER_33)]];
	device const TextureCoordinates *dynamic_uvs [[id(KILN_BUFFER_34)]];
	device const uint *grid_sums [[id(KILN_BUFFER_35)]];
};
kernel void main0(constant Resources &resources [[buffer(0)]], uint3 pixel [[thread_position_in_grid]], uint3 local [[thread_position_in_threadgroup]]) {
	Scene scene = { *resources.parameters, resources.depth, resources.normals, resources.triangles, resources.dynamic_triangles,
		resources.emitters, resources.surfels, resources.cell_heads, resources.cell_links, resources.local_lights, resources.light_grid,
		resources.tlas, resources.surface, resources.albedo, resources.albedo_sampler, resources.static_uvs, resources.dynamic_uvs, resources.grid_sums };
	auto base = resources.base;
	auto fresnel = resources.fresnel;
#else
kernel void main0(constant Parameters &parameters [[buffer(KILN_BUFFER_0)]], texture2d<float> depth [[texture(KILN_TEXTURE_1)]],
		texture2d<float> normals [[texture(KILN_TEXTURE_2)]], device const Triangle *triangles [[buffer(KILN_BUFFER_5)]],
		texture2d<float, access::write> base [[texture(KILN_TEXTURE_6)]], texture2d<float, access::write> fresnel [[texture(KILN_TEXTURE_7)]],
		device const Triangle *dynamic_triangles [[buffer(KILN_BUFFER_17)]], device const float4 *emitters [[buffer(KILN_BUFFER_18)]],
		device const Surfel *surfels [[buffer(KILN_BUFFER_20)]], device const uint2 *cell_heads [[buffer(KILN_BUFFER_21)]],
		device const uint *cell_links [[buffer(KILN_BUFFER_22)]], device const float4 *local_lights [[buffer(KILN_BUFFER_25)]],
		device const uint *light_grid [[buffer(KILN_BUFFER_26)]], instance_acceleration_structure tlas [[buffer(KILN_BUFFER_27)]],
		texture2d<uint> surface [[texture(KILN_TEXTURE_31)]], texture2d_array<float> albedo [[texture(KILN_TEXTURE_32)]],
		sampler albedo_sampler [[sampler(KILN_SAMPLER_32)]], device const TextureCoordinates *static_uvs [[buffer(KILN_BUFFER_33)]],
		device const TextureCoordinates *dynamic_uvs [[buffer(KILN_BUFFER_34)]], device const uint *grid_sums [[buffer(KILN_BUFFER_35)]],
		uint3 pixel [[thread_position_in_grid]], uint3 local [[thread_position_in_threadgroup]]) {
	Scene scene = { parameters, depth, normals, triangles, dynamic_triangles, emitters, surfels, cell_heads, cell_links,
		local_lights, light_grid, tlas, surface, albedo, albedo_sampler, static_uvs, dynamic_uvs, grid_sums };
#endif
	threadgroup float4 other_base[32], other_fresnel[32];
	uint index = local.y * 16u + local.x;
	Sample value = scene.sample(pixel.xy, local.z);
	if (local.z == 1u) { other_base[index] = value.base; other_fresnel[index] = value.fresnel; }
	threadgroup_barrier(mem_flags::mem_threadgroup);
	if (local.z == 0u && all(pixel.xy < uint2(scene.p.size_frame.xy))) {
		if (value.base.a >= 0.0f) { value.base += other_base[index]; value.fresnel += other_fresnel[index]; }
		base.write(value.base, pixel.xy);
		fresnel.write(value.fresnel, pixel.xy);
	}
}
