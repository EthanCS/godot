// Kiln engine integration. Engine licensing: LICENSE.txt.
// Imported algorithm provenance and redistribution limits: kiln/docs/gi-provenance.json.

#pragma once
#include "kiln_world.h"

#include "servers/rendering/renderer_rd/shaders/kiln_gi.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/kiln_specular_native.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_data_rd.h"

namespace RendererRD {
class KilnGI {
	enum Stage { SURFEL_UPDATE,
		SURFEL_GRID,
		SURFEL_GENERATE,
		SURFEL_TRACE,
		SURFEL_INTEGRATE,
		SURFEL_EVALUATE,
		SURFEL_PUBLISH,
		SURFEL_SPECULAR,
		SURFEL_SPECULAR_FILTER,
		SURFEL_DEBUG,
		SEQUENCE,
		AO_DEPTH,
		AO_MAIN,
		AO_DENOISE,
		AO_TEMPORAL,
		BVH_REFIT,
		SURFEL_GRID_PREFIX,
		SURFEL_GRID_PREFIX_SUMS,
		SURFEL_GRID_SCATTER,
		SURFEL_DIFFUSE_FILTER,
		STAGE_COUNT,
		QUERY_VALIDATE = STAGE_COUNT };
	KilnGiShaderRD shader, hardware_shader;
	RID hardware_version, hardware_pipelines[4];
	KilnSpecularNativeShaderRD native_specular_shader;
	RID native_specular_version;
	RID specular_pipelines[3][8];
	bool hardware_available = false;
	RID version, pipelines[STAGE_COUNT], sampler, linear_sampler, sequence, hilbert, empty_surface;
	struct Binding {
		int binding;
		RD::UniformType type;
		RID resource;
		bool linear = false;
	};
	void dispatch(Stage p_stage, Size2i p_size, std::initializer_list<Binding> p_bindings, int p_stride = 0, int p_z = 1, RID p_tlas = RID(), bool p_force_translated = false);
	RID texture(Size2i p_size, RD::DataFormat p_format);

public:
	class View : public RenderBufferCustomDataRD {
		GDCLASS(View, RenderBufferCustomDataRD);

	public:
		RenderSceneBuffersRD *buffers = nullptr;
		HashMap<String, RID> textures;
		Vector<RID> owned;
		HashMap<String, RID> storage;
		HashMap<String, uint32_t> capacities;
		Size2i size;
		uint64_t geometry_version = 0, dynamic_version = 0, light_version = 0, material_version = 0;
		uint64_t texture_version = 0;
		uint64_t static_material_version = 0, dynamic_material_version = 0;
		uint64_t capture_request = 0, history_version = 0;
		int frames = 0, index = 0, stationary_samples = 0, motion_remaining = 0, lighting_remaining = 0, epoch = 1;
		uint32_t slots = 65536;
		bool multibounce = true;
		Projection previous_vp, previous_projection;
		Transform3D previous_camera;
		bool ready = false, tracing = false;
		int ao_frames = 0, ao_quality = -1;
		RID parameters;
		RID ray_albedo;
		RID environment;
		RID hardware_vertices[2], hardware_blas[2], hardware_tlas;
		bool hardware_active = false, hardware_failed = false;
		uint64_t hardware_builds = 0, tlas_builds = 0;
		void free_hardware();
		RID t(const String &p_name, int p_index = 0) { return textures[p_name + itos(p_index)]; }
		void configure(RenderSceneBuffersRD *p_buffers) override {
			free_data();
			buffers = p_buffers;
		}
		void free_data() override;
		~View() { free_data(); }
	};
	bool process(Ref<RenderSceneBuffersRD> p_buffers, RenderSceneDataRD *p_scene, RID p_environment, RID p_normal, bool p_signed_normal = true);
	KilnGI();
	~KilnGI();
};
} //namespace RendererRD
