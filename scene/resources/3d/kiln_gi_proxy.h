// Copyright (c) Godot Engine contributors. See LICENSE.txt.
#pragma once
#ifdef RD_ENABLED
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"

// Serialized with an imported mesh; does not reference its owner (no resource cycle).
// Geometry is independent of texture detail. Source materials keep the solid-color
// proxy current in the editor and at runtime, even without a KilnGIWorld.
class KilnGIProxy : public Resource {
	GDCLASS(KilnGIProxy, Resource);
	Ref<ArrayMesh> proxy_mesh;
	TypedArray<Material> source_materials;
	Dictionary data;
	HashMap<ObjectID, Ref<Resource>> watched;
	HashMap<ObjectID, Color> averages;
	HashSet<ObjectID> used;
	uint64_t material_revision = 0;
	void observe(const Ref<Resource> &p_resource);
	void source_changed(ObjectID p_id);
	Color average(const Ref<Texture2D> &p_texture);

protected:
	static void _bind_methods();

public:
	static constexpr int FORMAT_VERSION = 1;
	static Dictionary simplify_surface(const Ref<Mesh> &p_mesh, int p_surface, float p_ratio, float p_error);
	static Ref<KilnGIProxy> generate(const Ref<Mesh> &p_mesh, float p_ratio = 0.12f, float p_error = 0.001f);
	static void attach(const Ref<Mesh> &p_mesh);
	bool matches(const Ref<Mesh> &p_mesh, int p_surface, float p_ratio, float p_error) const;
	Dictionary get_surface(int p_surface) const;
	void refresh_materials();
	void replace_source_material(int p_surface, const Ref<Material> &p_material);
	void set_proxy_mesh(const Ref<ArrayMesh> &p_mesh);
	Ref<ArrayMesh> get_proxy_mesh() const { return proxy_mesh; }
	void set_source_materials(const TypedArray<Material> &p_materials);
	TypedArray<Material> get_source_materials() const { return source_materials; }
	void set_data(const Dictionary &p_data) { data = p_data; }
	Dictionary get_data() const { return data; }
	uint64_t get_material_revision() const { return material_revision; }
	~KilnGIProxy();
};
#endif
