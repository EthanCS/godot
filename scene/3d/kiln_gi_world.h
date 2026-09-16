// Kiln engine integration. Engine licensing: LICENSE.txt.
// Imported algorithm provenance and redistribution limits: kiln/docs/gi-provenance.json.

#pragma once
#ifdef RD_ENABLED
#include "scene/3d/node_3d.h"
#include "scene/resources/environment.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "servers/rendering/renderer_rd/kiln/kiln_world.h"

// Native scene capture and software BVH owner. This is independent of compositors
// and material EMISSION injection; the renderer schedules the GPU work.
class KilnGIWorld : public Node3D {
	GDCLASS(KilnGIWorld, Node3D);
	struct Triangle {
		Vector3 a, b, c, normal, albedo, emission;
		int source = 0;
		Vector3 center() const { return (a + b + c) / 3; }
	};
	struct MeshData {
		PackedVector3Array positions, normals;
		PackedInt32Array indices;
		Vector3 vertex_color = Vector3(1, 1, 1);
		uint32_t source_triangles = 0;
		uint64_t revision = 0;
	};
	struct Transport {
		Vector3 albedo = Vector3(1, 1, 1), emission;
		bool supported = false;
	};
	Ref<Environment> environment;
	RendererRD::KilnWorld snapshot;
	HashMap<uint64_t, HashMap<int, MeshData>> mesh_cache;
	HashSet<ObjectID> unsupported_materials, unsupported_geometry;
	HashMap<ObjectID, Dictionary> shader_defaults;
	HashMap<ObjectID, Transport> material_cache;
	HashMap<ObjectID, Color> texture_cache;
	HashMap<ObjectID, Ref<Resource>> watched_resources;
	HashSet<ObjectID> used_resources;
	uint32_t static_hash = 0, dynamic_hash = 0, local_light_hash = 0;
	uint32_t static_material_hash = 0, dynamic_material_hash = 0;
	uint64_t proxy_builds = 0, material_updates = 0;
	uint64_t imported_proxy_hits = 0, runtime_proxy_builds = 0;
	float proxy_ratio = 0.12f, proxy_error = 0.001f;
	void watch(const Ref<Resource> &p_resource);
	void resource_changed(ObjectID p_id);
	Color texture_average(const Ref<Texture2D> &p_texture);
	Transport transport(const Ref<Material> &p_material);
	MeshData make_proxy(const Ref<Mesh> &p_mesh, int p_surface);
	void collect_lights(Node *p_node, Vector<Vector4> &r_lights, uint32_t &r_hash);
	void update_lights();
	bool rebuild_pending = true;
	void collect(Node *p_node, bool p_dynamic_parent, bool p_dynamic_pass, Vector<Triangle> &r_triangles, uint32_t &r_hash, uint32_t &r_material_hash, bool p_geometry = true);
	RendererRD::KilnWorld::Geometry build(Vector<Triangle> p_triangles);
	void pack_triangles(RendererRD::KilnWorld::Geometry &r_geometry, const Vector<Triangle> &p_triangles);

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void set_environment(const Ref<Environment> &p_environment);
	Ref<Environment> get_environment() const { return environment; }
	void rebuild() {
		rebuild_pending = true;
		mesh_cache.clear();
		shader_defaults.clear();
		material_cache.clear();
		texture_cache.clear();
	}
	void set_proxy_quality(float p_ratio, float p_error);
	void set_resolution_divisor(int p_divisor);
	void set_query_backend(int p_backend);
	void set_lighting(Vector3 p_direction, Vector3 p_color, float p_sun_energy, float p_sky_energy, float p_time_of_day);
	void set_enabled(bool p_enabled) { snapshot.enabled = p_enabled; }
	void set_ao_enabled(bool p_enabled) { snapshot.ao_quality = p_enabled ? 3 : 0; }
	void set_sky(Vector3 p_horizon, Vector3 p_zenith, bool p_procedural);
	void set_quality(int p_rays, int p_samples);
	void reset_history() { snapshot.history_version++; }
	void request_capture(const String &p_directory) {
		snapshot.capture_directory = p_directory;
		snapshot.capture_request++;
	}
	Dictionary get_statistics() const;
	Dictionary validate_bvh(int p_rays = 64) const;
	void set_profiling(bool p_enabled);
	bool is_enabled() const { return snapshot.enabled; }
	~KilnGIWorld();
};
#endif
