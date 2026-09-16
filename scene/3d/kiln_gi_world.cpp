// Kiln engine integration. Engine licensing: LICENSE.txt.
// Imported algorithm provenance and redistribution limits: kiln/docs/gi-provenance.json.

#include "kiln_gi_world.h"
#ifdef RD_ENABLED
#include "core/math/math_funcs.h"
#include "core/object/class_db.h"
#include "core/templates/hashfuncs.h"
#include "scene/3d/light_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/resources/material.h"
#include "servers/rendering/rendering_server.h"

#include <functional>

Dictionary KilnGIWorld::get_statistics() const {
	Dictionary result = environment.is_valid() ? RendererRD::KilnWorld::statistics(environment->get_rid()) : Dictionary();
	result["static_triangles"] = snapshot.world.triangle_count;
	result["dynamic_triangles"] = snapshot.dynamic.triangle_count;
	result["geometry_version"] = snapshot.geometry_version;
	result["dynamic_version"] = snapshot.dynamic_version;
	result["light_version"] = snapshot.light_version;
	result["local_lights"] = snapshot.local_light_count;
	result["backend"] = "software_bvh";
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
	snapshot.sky_horizon = p_horizon;
	snapshot.sky_zenith = p_zenith;
	snapshot.procedural_sky = p_procedural;
	snapshot.light_version++;
}
void KilnGIWorld::_bind_methods() {
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
void KilnGIWorld::collect(Node *p_node, bool p_dynamic_parent, bool p_dynamic_pass, Vector<Triangle> &r_triangles, uint32_t &r_hash) {
	if (p_node == this || bool(p_node->get_meta(SNAME("kiln_exclude"), false))) {
		return;
	}
	bool dynamic = p_dynamic_parent || bool(p_node->get_meta(SNAME("kiln_dynamic"), false));
	MeshInstance3D *instance = Object::cast_to<MeshInstance3D>(p_node);
	if (instance && dynamic == p_dynamic_pass && instance->is_visible_in_tree() && instance->get_mesh().is_valid()) {
		Ref<Mesh> mesh = instance->get_mesh();
		Transform3D transform = instance->get_global_transform();
		Basis normal_transform = transform.basis.inverse().transposed();
		r_hash = hash_murmur3_one_64(instance->get_instance_id(), r_hash);
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
				Array arrays = mesh->surface_get_arrays(surface);
				MeshData data;
				data.positions = arrays[Mesh::ARRAY_VERTEX];
				data.normals = arrays[Mesh::ARRAY_NORMAL];
				data.indices = arrays[Mesh::ARRAY_INDEX];
				mesh_cache[key].insert(surface, data);
			}
			const MeshData &data = mesh_cache[key][surface];
			Vector3 color(0.6, 0.6, 0.6), emission;
			Ref<Material> material = instance->get_active_material(surface);
			Ref<ShaderMaterial> shader_material = material;
			if (shader_material.is_valid()) {
				Variant authored = shader_material->get_shader_parameter(SNAME("tint_linear"));
				if (authored.get_type() == Variant::VECTOR3) {
					color = authored;
				}
				Variant e = shader_material->get_shader_parameter(SNAME("authored_emission"));
				if (e.get_type() == Variant::VECTOR3) {
					emission = e;
				}
			} else {
				Ref<BaseMaterial3D> base = material;
				if (base.is_valid()) {
					Color c = base->get_albedo().srgb_to_linear();
					color = Vector3(c.r, c.g, c.b);
				}
			}
			r_hash = hash_murmur3_one_32(Variant(color).hash(), r_hash);
			r_hash = hash_murmur3_one_32(Variant(emission).hash(), r_hash);
			int count = data.indices.is_empty() ? data.positions.size() : data.indices.size();
			for (int i = 0; i + 2 < count; i += 3) {
				int a = data.indices.is_empty() ? i : data.indices[i];
				int b = data.indices.is_empty() ? i + 1 : data.indices[i + 1];
				int c = data.indices.is_empty() ? i + 2 : data.indices[i + 2];
				Triangle tri;
				tri.a = transform.xform(data.positions[a]);
				tri.b = transform.xform(data.positions[b]);
				tri.c = transform.xform(data.positions[c]);
				Vector3 face = (tri.b - tri.a).cross(tri.c - tri.a);
				if (face.length_squared() < 1e-12) {
					continue;
				}
				tri.normal = data.normals.size() == data.positions.size() ? normal_transform.xform(data.normals[a] + data.normals[b] + data.normals[c]).normalized() : -face.normalized();
				float metal = shader_material.is_valid() ? float(shader_material->get_shader_parameter(SNAME("metalness"))) : 0.0f;
				tri.albedo = color.clamp(Vector3(), Vector3(1, 1, 1)) * (1.0f - CLAMP(metal, 0.0f, 1.0f));
				tri.emission = emission;
				r_triangles.push_back(tri);
			}
		}
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		collect(p_node->get_child(i), dynamic, p_dynamic_pass, r_triangles, r_hash);
	}
}

RendererRD::KilnWorld::Geometry KilnGIWorld::build(Vector<Triangle> triangles) {
	RendererRD::KilnWorld::Geometry result;
	struct Node {
		Vector3 lo, hi;
		int first, count, escape;
	};
	Vector<Node> nodes;
	Triangle *ordered = triangles.ptrw();
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
		nodes.push_back({ lo - Vector3(0.001, 0.001, 0.001), hi + Vector3(0.001, 0.001, 0.001), first, count, index + 1 });
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
	result.triangles.resize(MAX(1, triangles.size()) * 80);
	result.triangles.fill(0);
	result.nodes.resize(MAX(1, nodes.size()) * 32);
	result.nodes.fill(0);
	auto pack = [](float *p, Vector3 v, float w) { p[0] = v.x; p[1] = v.y; p[2] = v.z; p[3] = w; };
	float *out = reinterpret_cast<float *>(result.triangles.ptrw());
	for (int i = 0; i < triangles.size(); i++) {
		const Triangle &t = triangles[i];
		pack(out + i * 20, t.a, t.normal.x);
		pack(out + i * 20 + 4, t.b - t.a, t.normal.y);
		pack(out + i * 20 + 8, t.c - t.a, t.normal.z);
		pack(out + i * 20 + 12, t.albedo, 1);
		pack(out + i * 20 + 16, t.emission, 0);
		float luminance = t.emission.dot(Vector3(0.2126, 0.7152, 0.0722));
		float area = (t.b - t.a).cross(t.c - t.a).length() * 0.5;
		float weight = area * luminance;
		if (weight > 1e-10) {
			result.power += weight;
			result.emitters.push_back(Vector4(i, result.power, area, weight));
		}
	}
	out = reinterpret_cast<float *>(result.nodes.ptrw());
	for (int i = 0; i < nodes.size(); i++) {
		const Node &n = nodes[i];
		pack(out + i * 8, n.lo, n.count ? n.first : -1);
		pack(out + i * 8 + 4, n.hi, n.count ? -n.count - 1 : n.escape);
	}
	if (!nodes.is_empty()) {
		result.bounds = AABB(nodes[0].lo, nodes[0].hi - nodes[0].lo);
	}
	return result;
}
void KilnGIWorld::collect_lights(Node *node, Vector<Vector4> &lights, uint32_t &hash) {
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
	Vector3 origin = snapshot.world.bounds.position - Vector3(2, 2, 2);
	Vector3 extent = snapshot.world.bounds.size + Vector3(4, 4, 4);
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
	float *data = reinterpret_cast<float *>(snapshot.local_lights.ptrw());
	for (int i = 0; i < all.size(); i++) {
		for (int j = 0; j < 4; j++) {
			data[i * 4 + j] = all[i][j];
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
	if (rebuild_pending) {
		Vector<Triangle> triangles;
		uint32_t hash = 0;
		collect(get_parent(), false, false, triangles, hash);
		snapshot.world = build(triangles);
		snapshot.geometry_version++;
		rebuild_pending = false;
		print_line(vformat("[KILN_WORLD] static triangles=%d nodes=%d", snapshot.world.triangle_count, snapshot.world.node_count));
	}
	Vector<Triangle> dynamic;
	uint32_t hash = 0;
	collect(get_parent(), false, true, dynamic, hash);
	if (snapshot.dynamic_version == 0 || hash != dynamic_hash) {
		snapshot.dynamic = build(dynamic);
		snapshot.dynamic_version++;
		dynamic_hash = hash;
	}
	update_lights();
	RendererRD::KilnWorld::publish(environment->get_rid(), snapshot);
}
#endif
