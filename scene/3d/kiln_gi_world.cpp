// Kiln engine integration. Engine licensing: LICENSE.txt.
// Imported algorithm provenance and redistribution limits: kiln/docs/gi-provenance.json.

#include "kiln_gi_world.h"
#ifdef RD_ENABLED
#include "core/math/math_funcs.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/templates/hashfuncs.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/multimesh_instance_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/material.h"
#include "servers/rendering/rendering_server.h"

#include <functional>

Dictionary KilnGIWorld::get_statistics() const {
	Dictionary result = environment.is_valid() ? RendererRD::KilnWorld::statistics(environment->get_rid()) : Dictionary();
	result["bounds"] = snapshot.world.bounds;
	result["emissive_triangles"] = snapshot.world.emitters.size() + snapshot.dynamic.emitters.size();
	result["ao_quality"] = snapshot.ao_quality;
	result["static_triangles"] = snapshot.world.triangle_count;
	result["dynamic_triangles"] = snapshot.dynamic.triangle_count;
	result["geometry_version"] = snapshot.geometry_version;
	result["material_version"] = snapshot.material_version;
	result["history_version"] = snapshot.history_version;
	result["dynamic_version"] = snapshot.dynamic_version;
	result["light_version"] = snapshot.light_version;
	result["local_lights"] = snapshot.local_light_count;
	result["rays_per_frame"] = snapshot.rays;
	result["convergence_samples"] = snapshot.samples;
	result["backend"] = result.get("backend", "pending");
	result["requested_backend"] = snapshot.query_backend;
	result["mesh_uploads"] = mesh_uploads;
	result["material_updates"] = material_updates;
	uint64_t source_count = 0;
	for (const auto &mesh : mesh_cache) {
		for (const auto &surface : mesh.value) {
			source_count += surface.value.source_triangles;
		}
	}
	result["source_triangles_unique"] = source_count;
	result["ray_geometry"] = "original_scene_meshes";
	Array profile;
	auto areas = RenderingServer::get_singleton()->get_frame_profile();
	bool gpu_available = false;
	for (const auto &area : areas) {
		gpu_available |= area.gpu_msec > 0;
	}
	result["gpu_timestamps_available"] = gpu_available;
	for (const auto &area : areas) {
		Dictionary item;
		item["name"] = area.name;
		item["cpu_ms"] = area.cpu_msec;
		item["gpu_ms"] = gpu_available ? Variant(area.gpu_msec) : Variant();
		profile.push_back(item);
	}
	result["profile_frame"] = RenderingServer::get_singleton()->get_frame_profile_frame();
	result["profile"] = profile;
	return result;
}
void KilnGIWorld::set_profiling(bool p_enabled) {
	RenderingServer::get_singleton()->set_frame_profiling_enabled(p_enabled);
}
void KilnGIWorld::set_sky(Vector3 p_horizon, Vector3 p_zenith, bool p_procedural) {
	if (snapshot.sky_horizon == p_horizon && snapshot.sky_zenith == p_zenith && snapshot.procedural_sky == p_procedural) {
		return;
	}
	snapshot.sky_horizon = p_horizon;
	snapshot.sky_zenith = p_zenith;
	snapshot.procedural_sky = p_procedural;
	snapshot.light_version++;
}
void KilnGIWorld::set_capture_roots(const TypedArray<NodePath> &p_roots) {
	if (capture_roots == p_roots) {
		return;
	}
	capture_roots = p_roots.duplicate();
	// Root changes invalidate geometry, but keep shared mesh/texture uploads.
	rebuild_pending = true;
}
void KilnGIWorld::set_ao_quality(int p_quality) {
	ERR_FAIL_COND(p_quality < 0 || p_quality > 3);
	snapshot.ao_quality = p_quality;
}
void KilnGIWorld::set_sky_parameters(float p_halo, float p_saturation, Vector3 p_cloud_color, float p_cloud_coverage) {
	if (snapshot.sky_halo == p_halo && snapshot.sky_saturation == p_saturation && snapshot.cloud_color == p_cloud_color && snapshot.cloud_coverage == p_cloud_coverage) {
		return;
	}
	snapshot.sky_halo = p_halo;
	snapshot.sky_saturation = p_saturation;
	snapshot.cloud_color = p_cloud_color;
	snapshot.cloud_coverage = p_cloud_coverage;
	snapshot.light_version++;
}
void KilnGIWorld::collect_roots(bool p_dynamic_pass, Vector<Triangle> &r_triangles, uint32_t &r_hash, uint32_t &r_material_hash, bool p_geometry) {
	if (capture_roots.is_empty()) {
		collect(get_parent(), false, p_dynamic_pass, r_triangles, r_hash, r_material_hash, p_geometry);
		return;
	}
	HashSet<ObjectID> visited;
	for (int i = 0; i < capture_roots.size(); i++) {
		Node *root = get_node_or_null(capture_roots[i]);
		if (!root || visited.has(root->get_instance_id())) {
			continue;
		}
		bool covered = false;
		for (int j = 0; j < capture_roots.size(); j++) {
			Node *other = get_node_or_null(capture_roots[j]);
			if (other && other != root && other->is_ancestor_of(root)) {
				covered = true;
				break;
			}
		}
		if (!covered) {
			visited.insert(root->get_instance_id());
			collect(root, false, p_dynamic_pass, r_triangles, r_hash, r_material_hash, p_geometry);
		}
	}
}
void KilnGIWorld::set_quality(int p_rays, int p_samples) {
	ERR_FAIL_COND_MSG(p_rays < 1 || p_rays > 8 || p_samples < 16 || p_samples > 1024, "Kiln Surfel GI supports quality levels 1-8 (4-32 rays per updated surfel) and 16-1024 convergence samples.");
	if (snapshot.rays != p_rays || snapshot.samples != p_samples) {
		snapshot.rays = p_rays;
		snapshot.samples = p_samples;
		snapshot.history_version++;
	}
}
void KilnGIWorld::set_query_backend(int p_backend) {
	ERR_FAIL_COND_MSG(p_backend < 0 || p_backend > 2, "GI query backend must be 0 (automatic), 1 (software) or 2 (prefer hardware with fallback).");
	if (snapshot.query_backend != p_backend) {
		snapshot.query_backend = p_backend;
		snapshot.history_version++;
	}
}
void KilnGIWorld::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_capture_roots", "roots"), &KilnGIWorld::set_capture_roots);
	ClassDB::bind_method(D_METHOD("set_ao_quality", "quality"), &KilnGIWorld::set_ao_quality);
	ClassDB::bind_method(D_METHOD("set_sky_parameters", "halo", "saturation", "cloud_color", "cloud_coverage"), &KilnGIWorld::set_sky_parameters);
	ClassDB::bind_method(D_METHOD("set_query_backend", "backend"), &KilnGIWorld::set_query_backend);
	ClassDB::bind_method(D_METHOD("set_quality", "rays", "samples"), &KilnGIWorld::set_quality);
	ClassDB::bind_method(D_METHOD("validate_bvh", "rays"), &KilnGIWorld::validate_bvh, DEFVAL(64));
	ClassDB::bind_method(D_METHOD("set_profiling", "enabled"), &KilnGIWorld::set_profiling);
	ClassDB::bind_method(D_METHOD("reset_history"), &KilnGIWorld::reset_history);
	ClassDB::bind_method(D_METHOD("set_sky", "horizon", "zenith", "procedural"), &KilnGIWorld::set_sky);
	ClassDB::bind_method(D_METHOD("request_capture", "directory"), &KilnGIWorld::request_capture);
	ClassDB::bind_method(D_METHOD("get_statistics"), &KilnGIWorld::get_statistics);
	ClassDB::bind_method(D_METHOD("set_environment", "environment"), &KilnGIWorld::set_environment);
	ClassDB::bind_method(D_METHOD("get_environment"), &KilnGIWorld::get_environment);
	ClassDB::bind_method(D_METHOD("rebuild"), &KilnGIWorld::rebuild);
	ClassDB::bind_method(D_METHOD("set_lighting", "direction", "color", "sun_energy", "sky_energy", "time_of_day"), &KilnGIWorld::set_lighting);
	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &KilnGIWorld::set_enabled);
	ClassDB::bind_method(D_METHOD("set_ao_enabled", "enabled"), &KilnGIWorld::set_ao_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &KilnGIWorld::is_enabled);
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "environment", PROPERTY_HINT_RESOURCE_TYPE, "Environment"), "set_environment", "get_environment");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
}
void KilnGIWorld::set_environment(const Ref<Environment> &p_environment) {
	if (environment == p_environment) {
		return;
	}
	if (environment.is_valid()) {
		RendererRD::KilnWorld::remove(environment->get_rid());
	}
	environment = p_environment;
	rebuild_pending = true;
}
void KilnGIWorld::set_lighting(Vector3 p_direction, Vector3 p_color, float p_sun_energy, float p_sky_energy, float p_time_of_day) {
	if (snapshot.sun_direction != p_direction || snapshot.sun_color != p_color || snapshot.sun_energy != p_sun_energy || snapshot.sky_energy != p_sky_energy || snapshot.time_of_day != p_time_of_day) {
		snapshot.light_version++;
	}
	snapshot.sun_direction = p_direction.normalized();
	snapshot.sun_color = p_color;
	snapshot.sun_energy = p_sun_energy;
	snapshot.sky_energy = p_sky_energy;
	snapshot.time_of_day = p_time_of_day;
}
void KilnGIWorld::collect(Node *p_node, bool p_dynamic_parent, bool p_dynamic_pass, Vector<Triangle> &r_triangles, uint32_t &r_hash, uint32_t &r_material_hash, bool p_geometry) {
	if (p_node == this || (Object::cast_to<Viewport>(p_node) && p_node != get_viewport()) || bool(p_node->get_meta(SNAME("kiln_exclude"), false))) {
		return;
	}
	bool dynamic = p_dynamic_parent || bool(p_node->get_meta(SNAME("kiln_dynamic"), false));
	if (Object::cast_to<MultiMeshInstance3D>(p_node) && !unsupported_geometry.has(p_node->get_instance_id())) {
		unsupported_geometry.insert(p_node->get_instance_id());
		WARN_PRINT("Kiln GI omitted MultiMesh geometry: only rigid MeshInstance3D capture is supported.");
	}
	MeshInstance3D *instance = Object::cast_to<MeshInstance3D>(p_node);
	if (instance && (instance->get_skin().is_valid() || instance->get_blend_shape_count() > 0)) {
		if (!unsupported_geometry.has(instance->get_instance_id())) {
			unsupported_geometry.insert(instance->get_instance_id());
			WARN_PRINT("Kiln GI omitted skinned/blend-shape geometry: only rigid MeshInstance3D capture is supported.");
		}
		instance = nullptr;
	}
	if (instance && dynamic == p_dynamic_pass && instance->is_visible_in_tree() && instance->get_mesh().is_valid()) {
		Ref<Mesh> mesh = instance->get_mesh();
		watch(mesh);
		Transform3D transform = instance->get_global_transform();
		Basis normal_transform = transform.basis.inverse().transposed();
		r_hash = hash_murmur3_one_64(instance->get_instance_id(), r_hash);
		r_hash = hash_murmur3_one_64(mesh->get_instance_id(), r_hash);
		r_hash = hash_murmur3_one_32(Variant(transform).hash(), r_hash);
		for (int surface = 0; surface < mesh->get_surface_count(); surface++) {
			if (mesh->surface_get_primitive_type(surface) != Mesh::PRIMITIVE_TRIANGLES) {
				continue;
			}
			uint64_t key = uint64_t(mesh->get_instance_id());
			if (!mesh_cache.has(key)) {
				mesh_cache.insert(key, HashMap<int, MeshData>());
			}
			if (!mesh_cache[key].has(surface)) {
				mesh_cache[key].insert(surface, capture_mesh(mesh, surface));
			}
			const MeshData &geometry = mesh_cache[key][surface];
			Ref<Material> material = instance->get_active_material(surface);
			Transport response = transport(material);
			r_hash = hash_murmur3_one_64(geometry.revision, r_hash);
			r_hash = hash_murmur3_one_32(response.supported, r_hash);
			if (!response.supported) {
				continue;
			}
			Vector3 color = response.albedo, emission = response.emission;
			r_material_hash = hash_murmur3_one_32(Variant(color).hash(), r_material_hash);
			r_material_hash = hash_murmur3_one_32(Variant(emission).hash(), r_material_hash);
			r_material_hash = hash_murmur3_one_32(response.texture_page + 1, r_material_hash);
			r_material_hash = hash_murmur3_one_64(snapshot.texture_version, r_material_hash);
			r_material_hash = hash_murmur3_one_32(Variant(response.uv_scale).hash(), r_material_hash);
			r_material_hash = hash_murmur3_one_32(Variant(response.uv_offset).hash(), r_material_hash);
			r_material_hash = hash_murmur3_one_32(response.texture_repeat | (response.world_mapping << 1) | (response.vertex_color << 2), r_material_hash);
			if (!p_geometry) {
				continue;
			}
			int count = geometry.indices.is_empty() ? geometry.positions.size() : geometry.indices.size();
			for (int i = 0; i + 2 < count; i += 3) {
				int a = geometry.indices.is_empty() ? i : geometry.indices[i];
				int b = geometry.indices.is_empty() ? i + 1 : geometry.indices[i + 1];
				int c = geometry.indices.is_empty() ? i + 2 : geometry.indices[i + 2];
				if (a < 0 || b < 0 || c < 0 || a >= geometry.positions.size() || b >= geometry.positions.size() || c >= geometry.positions.size()) {
					continue;
				}
				Triangle tri;
				tri.a = transform.xform(geometry.positions[a]);
				tri.b = transform.xform(geometry.positions[b]);
				tri.c = transform.xform(geometry.positions[c]);
				Vector3 face = (tri.b - tri.a).cross(tri.c - tri.a);
				if (face.length_squared() < 1e-12) {
					continue;
				}
				tri.normal = geometry.normals.size() == geometry.positions.size() ? normal_transform.xform(geometry.normals[a] + geometry.normals[b] + geometry.normals[c]).normalized() : -face.normalized();
				tri.albedo = color;
				if (response.texture_page >= 0 && (response.world_mapping || geometry.uvs.size() == geometry.positions.size())) {
					auto uv = [&](int index) {
						Vector3 point = transform.xform(geometry.positions[index]);
						Vector2 coord = response.world_mapping ? Vector2(point.x, point.z) : geometry.uvs[index];
						return coord * response.uv_scale + response.uv_offset;
					};
					tri.uv_a = uv(a);
					tri.uv_b = uv(b);
					tri.uv_c = uv(c);
					tri.texture_page = response.texture_page;
					tri.texture_repeat = response.texture_repeat;
				} else if (response.texture_page >= 0) {
					tri.albedo *= response.texture_fallback;
				}
				if (response.vertex_color && geometry.colors.size() == geometry.positions.size()) {
					Color vertex_color = (geometry.colors[a].srgb_to_linear() + geometry.colors[b].srgb_to_linear() + geometry.colors[c].srgb_to_linear()) / 3.0;
					tri.albedo *= Vector3(vertex_color.r, vertex_color.g, vertex_color.b);
				}
				tri.emission = emission;
				r_triangles.push_back(tri);
			}
		}
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		collect(p_node->get_child(i), dynamic, p_dynamic_pass, r_triangles, r_hash, r_material_hash, p_geometry);
	}
}

RendererRD::KilnWorld::Geometry KilnGIWorld::build(Vector<Triangle> triangles) {
	RendererRD::KilnWorld::Geometry result;
	struct Node {
		Vector3 lo, hi;
		int first, count, escape, depth;
	};
	Vector<Node> nodes;
	Triangle *ordered = triangles.ptrw();
	for (int i = 0; i < triangles.size(); i++) {
		ordered[i].source = i;
	}
	std::function<void(int, int, int)> split = [&](int first, int count, int depth) {
		Vector3 lo(INFINITY, INFINITY, INFINITY), hi(-INFINITY, -INFINITY, -INFINITY), clo = lo, chi = hi;
		for (int i = first; i < first + count; i++) {
			const Triangle &t = ordered[i];
			lo = lo.min(t.a).min(t.b).min(t.c);
			hi = hi.max(t.a).max(t.b).max(t.c);
			clo = clo.min(t.center());
			chi = chi.max(t.center());
		}
		int index = nodes.size();
		nodes.push_back({ lo - Vector3(0.001, 0.001, 0.001), hi + Vector3(0.001, 0.001, 0.001), first, count, index + 1, depth });
		if (count <= 8 || depth >= 30) {
			return;
		}
		struct Bin {
			Vector3 lo = Vector3(INFINITY, INFINITY, INFINITY), hi = Vector3(-INFINITY, -INFINITY, -INFINITY);
			int count = 0;
		};
		auto area = [](Vector3 a, Vector3 b) { Vector3 e = (b - a).max(Vector3()); return e.x * e.y + e.y * e.z + e.z * e.x; };
		int best_axis = -1, best_bin = -1;
		float best_cost = INFINITY;
		const int bin_count = count <= 32 ? 8 : 16;
		for (int axis = 0; axis < 3; axis++) {
			float extent = chi[axis] - clo[axis];
			if (extent < 1e-6) {
				continue;
			}
			Bin bins[16];
			for (int i = first; i < first + count; i++) {
				const Triangle &t = ordered[i];
				Bin &bin = bins[CLAMP(int((t.center()[axis] - clo[axis]) * bin_count / extent), 0, bin_count - 1)];
				bin.count++;
				bin.lo = bin.lo.min(t.a).min(t.b).min(t.c);
				bin.hi = bin.hi.max(t.a).max(t.b).max(t.c);
			}
			float prefix[16];
			Bin accum;
			for (int b = 0; b < bin_count - 1; b++) {
				if (bins[b].count) {
					accum.lo = accum.lo.min(bins[b].lo);
					accum.hi = accum.hi.max(bins[b].hi);
					accum.count += bins[b].count;
				}
				prefix[b] = accum.count ? area(accum.lo, accum.hi) * accum.count : INFINITY;
			}
			accum = Bin();
			for (int b = bin_count - 1; b > 0; b--) {
				if (bins[b].count) {
					accum.lo = accum.lo.min(bins[b].lo);
					accum.hi = accum.hi.max(bins[b].hi);
					accum.count += bins[b].count;
				}
				float cost = accum.count ? prefix[b - 1] + area(accum.lo, accum.hi) * accum.count : INFINITY;
				if (cost < best_cost) {
					best_cost = cost;
					best_axis = axis;
					best_bin = b - 1;
				}
			}
		}
		if (best_axis < 0) {
			return;
		}
		int middle = first;
		for (int i = first; i < first + count; i++) {
			int bin = CLAMP(int((ordered[i].center()[best_axis] - clo[best_axis]) * bin_count / (chi[best_axis] - clo[best_axis])), 0, bin_count - 1);
			if (bin <= best_bin) {
				SWAP(ordered[middle], ordered[i]);
				middle++;
			}
		}
		if (middle == first || middle == first + count) {
			return;
		}
		split(first, middle - first, depth + 1);
		split(middle, first + count - middle, depth + 1);
		nodes.write[index].count = 0;
		nodes.write[index].escape = nodes.size();
	};
	if (!triangles.is_empty()) {
		split(0, triangles.size(), 1);
	}
	result.triangle_count = triangles.size();
	result.node_count = nodes.size();
	result.source_order.resize(triangles.size());
	for (int i = 0; i < triangles.size(); i++) {
		result.source_order.set(i, triangles[i].source);
	}
	// Pack once in traversal order. Later color/emission edits reuse this order
	// and the exact hierarchy, without touching the geometry revision.
	PackedInt32Array order = result.source_order;
	for (int i = 0; i < triangles.size(); i++) {
		result.source_order.set(i, i);
	}
	pack_triangles(result, triangles);
	result.source_order = order;
	result.nodes.resize(MAX(1, nodes.size()) * 32);
	result.nodes.fill(0);
	auto pack = [](float *p, Vector3 v, float w) { p[0] = v.x; p[1] = v.y; p[2] = v.z; p[3] = w; };
	float *out;
	out = reinterpret_cast<float *>(result.nodes.ptrw());
	for (int i = 0; i < nodes.size(); i++) {
		const Node &n = nodes[i];
		pack(out + i * 8, n.lo, n.count ? n.first : -1);
		pack(out + i * 8 + 4, n.hi, n.count ? -n.count - 1 : n.escape);
	}
	result.refit_order.resize(MAX(1, nodes.size()) * 4);
	uint32_t *refit = reinterpret_cast<uint32_t *>(result.refit_order.ptrw());
	int offset = 0;
	for (int depth = 30; depth >= 1; depth--) {
		int start = offset;
		for (int i = 0; i < nodes.size(); i++) {
			if (nodes[i].depth == depth) {
				refit[offset++] = i;
			}
		}
		if (offset > start) {
			result.refit_levels.push_back(Vector2i(start, offset - start));
		}
	}
	if (!nodes.is_empty()) {
		result.bounds = AABB(nodes[0].lo, nodes[0].hi - nodes[0].lo);
	}
	return result;
}
void KilnGIWorld::pack_triangles(RendererRD::KilnWorld::Geometry &result, const Vector<Triangle> &triangles) {
	ERR_FAIL_COND(result.source_order.size() != triangles.size());
	result.emitters.clear();
	result.power = 0;
	result.triangles.resize(MAX(1, triangles.size()) * 80);
	result.triangles.fill(0);
	result.texture_coordinates.resize(MAX(1, triangles.size()) * 32);
	result.texture_coordinates.fill(0);
	float *uvs = reinterpret_cast<float *>(result.texture_coordinates.ptrw());
	auto pack = [](float *p, Vector3 v, float w) { p[0] = v.x; p[1] = v.y; p[2] = v.z; p[3] = w; };
	float *out = reinterpret_cast<float *>(result.triangles.ptrw());
	for (int i = 0; i < triangles.size(); i++) {
		const Triangle &t = triangles[result.source_order[i]];
		pack(out + i * 20, t.a, t.normal.x);
		pack(out + i * 20 + 4, t.b - t.a, t.normal.y);
		pack(out + i * 20 + 8, t.c - t.a, t.normal.z);
		pack(out + i * 20 + 12, t.albedo, 1);
		pack(out + i * 20 + 16, t.emission, 0);
		uvs[i * 8] = t.uv_a.x;
		uvs[i * 8 + 1] = t.uv_a.y;
		uvs[i * 8 + 2] = t.uv_b.x;
		uvs[i * 8 + 3] = t.uv_b.y;
		uvs[i * 8 + 4] = t.uv_c.x;
		uvs[i * 8 + 5] = t.uv_c.y;
		uvs[i * 8 + 6] = t.texture_page + 1;
		uvs[i * 8 + 7] = t.texture_repeat;
		float luminance = t.emission.dot(Vector3(0.2126, 0.7152, 0.0722));
		float area = (t.b - t.a).cross(t.c - t.a).length() * 0.5;
		float weight = area * luminance;
		if (weight > 1e-10) {
			result.power += weight;
			result.emitters.push_back(Vector4(i, result.power, area, weight));
		}
	}
}
Dictionary KilnGIWorld::validate_bvh(int p_rays) const {
	ERR_FAIL_COND_V(p_rays < 1 || p_rays > 1024, Dictionary());
	int failures = 0, tested = 0;
	float maximum_error = 0;
	uint32_t seed = 214;
	auto random = [&]() { seed = seed * 1664525u + 1013904223u; return float(seed >> 8) / 16777216.0f; };
	for (const auto *geometry : { &snapshot.world, &snapshot.dynamic }) {
		if (geometry->triangle_count == 0) {
			continue;
		}
		const float *triangles = reinterpret_cast<const float *>(geometry->triangles.ptr());
		const float *nodes = reinterpret_cast<const float *>(geometry->nodes.ptr());
		auto vec = [](const float *p) { return Vector3(p[0], p[1], p[2]); };
		for (int ray = 0; ray < p_rays; ray++) {
			Vector3 origin = geometry->bounds.get_center() + (Vector3(random(), random(), random()) * 2 - Vector3(1, 1, 1)) * geometry->bounds.size;
			Vector3 target = geometry->bounds.position + Vector3(random(), random(), random()) * geometry->bounds.size;
			Vector3 direction = (target - origin).normalized();
			auto intersect = [&](int index, float maximum) {
				const float *t = triangles + index * 20;
				Vector3 e1 = vec(t + 4), e2 = vec(t + 8), h = direction.cross(e2);
				float det = e1.dot(h);
				if (Math::abs(det) < 1e-9f) {
					return maximum;
				}
				Vector3 s = origin - vec(t);
				float u = s.dot(h) / det;
				if (u < 0 || u > 1) {
					return maximum;
				}
				Vector3 q = s.cross(e1);
				float v = direction.dot(q) / det;
				if (v < 0 || u + v > 1) {
					return maximum;
				}
				float distance = e2.dot(q) / det;
				return distance > 0.006f && distance < maximum ? distance : maximum;
			};
			float brute = 100000, accelerated = brute;
			for (uint32_t i = 0; i < geometry->triangle_count; i++) {
				brute = intersect(i, brute);
			}
			uint32_t index = 0;
			while (index < geometry->node_count) {
				const float *n = nodes + index * 8;
				float near = 0, far = accelerated;
				for (int axis = 0; axis < 3; axis++) {
					float inverse = 1.0f / (direction[axis] + (direction[axis] < 0 ? -1e-8f : 1e-8f));
					float a = (n[axis] - origin[axis]) * inverse, b = (n[axis + 4] - origin[axis]) * inverse;
					near = MAX(near, MIN(a, b));
					far = MIN(far, MAX(a, b));
				}
				bool leaf = n[7] < 0;
				if (near > far) {
					index = leaf ? index + 1 : uint32_t(n[7]);
					continue;
				}
				if (leaf) {
					for (int i = int(n[3]); i < int(n[3]) - int(n[7]) - 1; i++) {
						accelerated = intersect(i, accelerated);
					}
				}
				index++;
			}
			float error = Math::abs(brute - accelerated);
			maximum_error = MAX(maximum_error, error);
			if (error > 0.002f) {
				failures++;
			}
			tested++;
		}
	}
	Dictionary result;
	result["rays"] = tested;
	result["failures"] = failures;
	result["maximum_error"] = maximum_error;
	result["passed"] = tested > 0 && failures == 0;
	return result;
}
void KilnGIWorld::collect_lights(Node *node, Vector<Vector4> &lights, uint32_t &hash) {
	if ((Object::cast_to<Viewport>(node) && node != get_viewport()) || bool(node->get_meta(SNAME("kiln_exclude"), false))) {
		return;
	}
	Light3D *light = Object::cast_to<Light3D>(node);
	if (light && light->is_visible_in_tree() && !Object::cast_to<DirectionalLight3D>(light)) {
		if (Object::cast_to<OmniLight3D>(light) || Object::cast_to<SpotLight3D>(light)) {
			Vector3 position = light->get_global_position();
			Vector3 direction = -light->get_global_basis().get_column(2).normalized();
			Color color = light->get_color().srgb_to_linear();
			float energy = light->get_param(Light3D::PARAM_ENERGY) * light->get_param(Light3D::PARAM_INDIRECT_ENERGY) * Math::PI;
			if (light->is_negative()) {
				energy = 0; // Signed light transport is outside the Kiln contract.
			}
			bool spot = Object::cast_to<SpotLight3D>(light) != nullptr;
			Vector4 values[] = {
				Vector4(position.x, position.y, position.z, light->get_param(Light3D::PARAM_RANGE)),
				Vector4(color.r * energy, color.g * energy, color.b * energy, light->get_param(Light3D::PARAM_ATTENUATION)),
				Vector4(direction.x, direction.y, direction.z, spot ? Math::cos(Math::deg_to_rad(light->get_param(Light3D::PARAM_SPOT_ANGLE))) : -2.0),
				Vector4(1.0 / MAX(0.0001, light->get_param(Light3D::PARAM_SPOT_ATTENUATION)), 0, 0, 0)
			};
			for (const Vector4 &v : values) {
				lights.push_back(v);
				hash = hash_murmur3_one_32(Variant(v).hash(), hash);
			}
		}
	}
	for (int i = 0; i < node->get_child_count(); i++) {
		collect_lights(node->get_child(i), lights, hash);
	}
}
void KilnGIWorld::update_lights() {
	Vector<Vector4> lights;
	uint32_t hash = 0;
	collect_lights(get_parent(), lights, hash);
	if (hash == local_light_hash && !snapshot.local_lights.is_empty()) {
		return;
	}
	local_light_hash = hash;
	snapshot.light_version++;
	snapshot.local_light_count = lights.size() / 4;
	AABB bounds = snapshot.world.triangle_count ? snapshot.world.bounds : snapshot.dynamic.bounds;
	if (snapshot.dynamic.triangle_count) {
		bounds.merge_with(snapshot.dynamic.bounds);
	}
	Vector3 origin = bounds.position - Vector3(2, 2, 2);
	Vector3 extent = bounds.size + Vector3(4, 4, 4);
	float cell_size = MAX(8.0f, extent[extent.max_axis_index()] / 48.0f);
	Vector3i dims = Vector3i((extent / cell_size).ceil());
	int cell_count = dims.x * dims.y * dims.z;
	Vector<Vector<int>> cells;
	cells.resize(cell_count);
	for (int i = 0; i < int(snapshot.local_light_count); i++) {
		Vector4 pr = lights[i * 4];
		Vector3 pos(pr.x, pr.y, pr.z), range(pr.w, pr.w, pr.w);
		Vector3i lo = Vector3i(((pos - range - origin) / cell_size).floor()).max(Vector3i());
		Vector3i hi = Vector3i(((pos + range - origin) / cell_size).floor()).min(dims - Vector3i(1, 1, 1));
		for (int z = lo.z; z <= hi.z; z++) {
			for (int y = lo.y; y <= hi.y; y++) {
				for (int x = lo.x; x <= hi.x; x++) {
					cells.write[(z * dims.y + y) * dims.x + x].push_back(i);
				}
			}
		}
	}
	int entries = cell_count * 2;
	for (const Vector<int> &cell : cells) {
		entries += cell.size();
	}
	snapshot.light_grid.resize(MAX(entries, 2) * 4);
	uint32_t *grid = reinterpret_cast<uint32_t *>(snapshot.light_grid.ptrw());
	int offset = cell_count * 2;
	for (int i = 0; i < cell_count; i++) {
		grid[i * 2] = offset;
		grid[i * 2 + 1] = cells[i].size();
		for (int id : cells[i]) {
			grid[offset++] = id;
		}
	}
	Vector<Vector4> all;
	all.push_back(Vector4(origin.x, origin.y, origin.z, cell_size));
	all.push_back(Vector4(dims.x, dims.y, dims.z, snapshot.local_light_count));
	all.append_array(lights);
	snapshot.local_lights.resize(all.size() * 16);
	float *packed_lights = reinterpret_cast<float *>(snapshot.local_lights.ptrw());
	for (int i = 0; i < all.size(); i++) {
		for (int j = 0; j < 4; j++) {
			packed_lights[i * 4 + j] = all[i][j];
		}
	}
}

void KilnGIWorld::_notification(int p_what) {
	if (p_what == NOTIFICATION_READY) {
		set_process_internal(true);
	}
	if (p_what == NOTIFICATION_EXIT_TREE && environment.is_valid()) {
		RendererRD::KilnWorld::remove(environment->get_rid());
	}
	if (p_what != NOTIFICATION_INTERNAL_PROCESS || environment.is_null() || !get_parent()) {
		return;
	}
	used_resources.clear();
	bool changed_bounds = false;
	for (bool dynamic_pass : { false, true }) {
		Vector<Triangle> triangles;
		uint32_t hash = 0, material_hash = 0;
		collect_roots(dynamic_pass, triangles, hash, material_hash, false);
		uint32_t &previous_hash = dynamic_pass ? dynamic_hash : static_hash;
		uint32_t &previous_material = dynamic_pass ? dynamic_material_hash : static_material_hash;
		uint64_t &version = dynamic_pass ? snapshot.dynamic_version : snapshot.geometry_version;
		auto &geometry = dynamic_pass ? snapshot.dynamic : snapshot.world;
		bool geometry_changed = rebuild_pending || version == 0 || hash != previous_hash;
		if (geometry_changed || material_hash != previous_material) {
			uint32_t unused_hash = 0, unused_material = 0;
			collect_roots(dynamic_pass, triangles, unused_hash, unused_material, true);
			if (geometry_changed) {
				geometry = build(triangles);
				version++;
				changed_bounds = true;
			} else {
				pack_triangles(geometry, triangles);
			}
			if (material_hash != previous_material) {
				snapshot.material_version++;
				(dynamic_pass ? snapshot.dynamic_material_version : snapshot.static_material_version)++;
			}
			previous_material = material_hash;
			previous_hash = hash;
		}
	}
	if (rebuild_pending) {
		print_line(vformat("[KILN_WORLD] original scene triangles=%d nodes=%d", snapshot.world.triangle_count, snapshot.world.node_count));
	}
	rebuild_pending = false;
	Vector<ObjectID> unused;
	for (const auto &entry : watched_resources) {
		if (!used_resources.has(entry.key)) {
			unused.push_back(entry.key);
		}
	}
	for (ObjectID id : unused) {
		watched_resources[id]->disconnect_changed(callable_mp(this, &KilnGIWorld::resource_changed).bind(id));
		watched_resources.erase(id);
		mesh_cache.erase(uint64_t(id));
		texture_cache.erase(id);
		if (texture_pages.has(id)) {
			free_texture_pages.push_back(texture_pages[id]);
			texture_pages.erase(id);
			dirty_textures.erase(id);
		}
		material_cache.erase(id);
		shader_defaults.erase(id);
	}
	if (changed_bounds) {
		// Light cells must also follow the geometry's bounds when no light moved.
		snapshot.local_lights.clear();
	}
	update_lights();
	RendererRD::KilnWorld::publish(environment->get_rid(), snapshot);
}
#endif
