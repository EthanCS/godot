// Kiln's native RenderingDevice adapter. Engine licensing: LICENSE.txt.
#pragma once

#include "servers/rendering/renderer_rd/storage_rd/render_scene_data_rd.h"

namespace RendererRD {
class KilnNRD {
	struct Impl;
	Impl *impl = nullptr;

public:
	static bool available();
	bool initialize(Size2i p_size, bool p_specular, bool p_combined_specular);
	// Downstream RELAX: full-rate material-independent cache diffuse, plus optional
	// Schlick integrals with real hit distances. Diffuse disables hit-distance-based
	// preblur/reconstruction; its unused alpha is zero, never a fabricated distance.
	bool denoise(RenderSceneDataRD *p_scene, const Projection &p_previous_projection, const Transform3D &p_previous_camera, uint32_t p_frame, bool p_reset, bool p_changing, bool p_checkerboard, int p_diffuse_iterations, RID p_motion, RID p_normal, RID p_depth, RID p_diffuse, RID p_base, RID p_fresnel, RID p_out_diffuse, RID p_out_base, RID p_out_fresnel);
	~KilnNRD();
};
} //namespace RendererRD
