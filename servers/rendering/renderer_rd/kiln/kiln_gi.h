// Kiln engine integration. Engine licensing: LICENSE.txt.
// Imported algorithm provenance and redistribution limits: kiln/docs/gi-provenance.json.

#pragma once
#include "kiln_world.h"

#include "servers/rendering/renderer_rd/shaders/kiln_gi.glsl.gen.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_data_rd.h"

namespace RendererRD {
class KilnGI {
	enum Stage { PREPARE,
		TRACE,
		INTEGRATE,
		TEMPORAL,
		WORLD_CACHE,
		FILTER,
		DECODE,
		BRDF,
		SEQUENCE,
		PUBLISH,
		AO_DEPTH,
		AO_MAIN,
		AO_DENOISE,
		AO_TEMPORAL,
		STAGE_COUNT };
	KilnGiShaderRD shader;
	RID version, pipelines[STAGE_COUNT], sampler, linear_sampler, brdf, sequence, hilbert;
	struct Binding {
		int binding;
		RD::UniformType type;
		RID resource;
		bool linear = false;
	};
	void dispatch(Stage p_stage, Size2i p_size, std::initializer_list<Binding> p_bindings, int p_stride = 0, int p_z = 1);
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
		Size2i size, half;
		uint64_t geometry_version = 0, dynamic_version = 0, light_version = 0;
		uint64_t capture_request = 0, history_version = 0;
		int frames = 0, index = 0, stationary_samples = 0, motion_remaining = 0, lighting_remaining = 0, epoch = 1;
		uint32_t slots = 65536, dirty_bytes = 0;
		Projection previous_vp, previous_projection;
		Transform3D previous_camera;
		bool ready = false, tracing = false;
		int ao_frames = 0, ao_quality = -1;
		RID parameters;
		RID environment;
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
