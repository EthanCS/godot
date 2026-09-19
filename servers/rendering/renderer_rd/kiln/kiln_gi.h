// Kiln engine integration. Engine licensing: LICENSE.txt.
// Imported algorithm provenance and redistribution limits: kiln/provenance/gi-provenance.json.

#pragma once
#include "kiln_world.h"

#include "servers/rendering/renderer_rd/shaders/kiln_gi.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_data_rd.h"

#include <vector>

namespace RendererRD {
class KilnGI {
	enum Stage {
		BVH_REFIT,
		KILN_SKY,
		KILN_SURFEL_CLEAR_POOL,
		KILN_SURFEL_FIND_MISSING,
		KILN_SURFEL_ARGS,
		KILN_SURFEL_AGE,
		KILN_SURFEL_ALLOCATE,
		KILN_SURFEL_CLEAR_CELLS,
		KILN_SURFEL_COUNT_CELLS,
		KILN_SURFEL_SCAN,
		KILN_SURFEL_SCAN_SEGMENTS,
		KILN_SURFEL_SCAN_MERGE,
		KILN_SURFEL_SLOT_CELLS,
		KILN_SURFEL_TRACE,
		KILN_RESTIR_TRACE,
		KILN_RESTIR_TEMPORAL,
		KILN_RESTIR_SPATIAL,
		KILN_RESTIR_RESOLVE,
		KILN_BRDF_LUT,
		KILN_LIGHT,
		KILN_RTDGI_REPROJECT,
		KILN_RTDGI_TEMPORAL_FILTER,
		KILN_RTDGI_SPATIAL_FILTER,
		KILN_RTDGI_VALIDITY,
		KILN_RTR_TRACE,
		KILN_RTR_TEMPORAL,
		KILN_RTR_RESOLVE,
		KILN_RTR_FILTER,
		KILN_RTR_CLEANUP,
		KILN_SSGI,
		KILN_SSGI_SPATIAL,
		KILN_SSGI_UPSAMPLE,
		KILN_SSGI_TEMPORAL,
		KILN_SHADOW_TRACE,
		KILN_SHADOW_BITPACK,
		KILN_SHADOW_TEMPORAL,
		KILN_SHADOW_SPATIAL,
		KILN_TAA_REPROJECT,
		KILN_TAA_INPUT,
		KILN_TAA_HISTORY,
		KILN_TAA_PROB,
		KILN_TAA_PROB_FILTER,
		KILN_TAA_PROB_FILTER2,
		KILN_TAA,
		KILN_DISPLAY_LUT,
		KILN_POST,
		KILN_POST_BLUR0,
		KILN_POST_BLUR,
		KILN_POST_REVERSE,
		KILN_RTDGI_HISTORY_REPROJECT,
		KILN_WRC_TRACE,
		KILN_VELOCITY_REDUCE_X,
		KILN_VELOCITY_REDUCE_Y,
		KILN_VELOCITY_DILATE,
		KILN_MOTION_BLUR,
		STAGE_COUNT
	};
	KilnGiShaderRD shader, hardware_shader;
	static constexpr int HARDWARE_STAGE_COUNT = 6;
	RID hardware_version, hardware_pipelines[HARDWARE_STAGE_COUNT];
	bool hardware_available = false;
	RID version, pipelines[STAGE_COUNT], sampler, linear_sampler, linear_clamp_sampler, blue_noise, rtr_noise;
	struct Binding {
		int binding;
		RD::UniformType type;
		RID resource;
		bool linear = false;
		bool clamp = false;
	};
	void dispatch(Stage p_stage, Size2i p_size, const std::vector<Binding> &p_bindings, int p_stride = 0, int p_z = 1, RID p_tlas = RID());
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
		uint64_t geometry_version = 0, dynamic_version = 0, material_version = 0;
		uint64_t texture_version = 0;
		uint64_t static_material_version = 0, dynamic_material_version = 0;
		uint64_t capture_request = 0, history_version = 0;
		int frames = 0, index = 0;
		Projection previous_projection;
		Transform3D previous_camera;
		Vector2 previous_jitter;
		bool ready = false, tracing = false;
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
		void free_cache();
		~View() { free_cache(); }
	};
	bool process(Ref<RenderSceneBuffersRD> p_buffers, RenderSceneDataRD *p_scene, RID p_environment, RID p_normal);
	RID process_display(Ref<RenderSceneBuffersRD> p_buffers, RID p_color);
	KilnGI();
	~KilnGI();
};
} //namespace RendererRD
