// Kiln engine integration. Engine licensing: LICENSE.txt.
// Imported algorithm provenance and redistribution limits: kiln/docs/gi-provenance.json.

#pragma once
#ifdef RD_ENABLED
#include "scene/3d/node_3d.h"
#include "scene/resources/environment.h"
#include "servers/rendering/renderer_rd/kiln/kiln_world.h"

// Native scene capture and software BVH owner. This is independent of compositors
// and material EMISSION injection; the renderer schedules the GPU work.
class KilnGIWorld : public Node3D {
	GDCLASS(KilnGIWorld, Node3D);
	struct Triangle {
		Vector3 a, b, c, normal, albedo, emission;
		Vector3 center() const { return (a + b + c) / 3; }
	};
	struct MeshData {
		PackedVector3Array positions, normals;
		PackedInt32Array indices;
	};
	Ref<Environment> environment;
	RendererRD::KilnWorld snapshot;
	HashMap<uint64_t, HashMap<int, MeshData>> mesh_cache;
	HashSet<ObjectID> unsupported_materials;
	HashMap<ObjectID, Dictionary> shader_defaults;
	uint32_t dynamic_hash = 0, local_light_hash = 0;
	void collect_lights(Node *p_node, Vector<Vector4> &r_lights, uint32_t &r_hash);
	void update_lights();
	bool rebuild_pending = true;
	void collect(Node *p_node, bool p_dynamic_parent, bool p_dynamic_pass, Vector<Triangle> &r_triangles, uint32_t &r_hash);
	RendererRD::KilnWorld::Geometry build(Vector<Triangle> p_triangles);

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
	}
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
};
#endif
