/*
 * AutoJernih - filter video OBS dengan koreksi otomatis
 *
 * Tiap beberapa frame, filter mengecilkan gambar (64x36), menghitung rata-rata
 * warna & kecerahan di CPU, lalu menyetel gain warna (white balance) dan
 * exposure secara halus. Saat kamera di mixer berganti dan warnanya melompat,
 * filter beradaptasi lebih cepat. Ketajaman dan kontras diterapkan di shader.
 *
 * Tidak butuh GPU NVIDIA.
 */

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>
#include <math.h>

OBS_DECLARE_MODULE()

MODULE_EXPORT const char *obs_module_description(void)
{
	return "AutoJernih: koreksi warna, exposure, dan ketajaman otomatis";
}

#define AJ_W 64
#define AJ_H 36
#define AJ_EVERY 6            /* analisis tiap 6 frame */
#define AJ_TARGET_LUMA 118.0f /* target kecerahan rata-rata (0-255) */

struct aj_data {
	obs_source_t *ctx;

	gs_effect_t *effect;
	gs_eparam_t *p_gain;
	gs_eparam_t *p_contrast;
	gs_eparam_t *p_saturation;
	gs_eparam_t *p_sharp;
	gs_eparam_t *p_texel;

	gs_texrender_t *render;
	gs_stagesurf_t *stage;

	/* pengaturan */
	float strength;
	float sharpness;
	float speed;
	bool auto_wb;
	bool auto_exp;

	/* status (nilai halus yang dipakai shader) */
	float gain[3];
	float exposure;
	float last_luma;
	uint32_t frame;
};

static inline float aj_clamp(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static const char *aj_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "AutoJernih (Warna & Ketajaman Otomatis)";
}

static void aj_update(void *data, obs_data_t *settings)
{
	struct aj_data *d = data;
	d->strength = (float)obs_data_get_double(settings, "strength");
	d->sharpness = (float)obs_data_get_double(settings, "sharpness");
	d->speed = (float)obs_data_get_double(settings, "speed");
	d->auto_wb = obs_data_get_bool(settings, "auto_wb");
	d->auto_exp = obs_data_get_bool(settings, "auto_exp");
}

static void aj_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, "strength", 0.7);
	obs_data_set_default_double(settings, "sharpness", 0.3);
	obs_data_set_default_double(settings, "speed", 0.5);
	obs_data_set_default_bool(settings, "auto_wb", true);
	obs_data_set_default_bool(settings, "auto_exp", true);
}

static obs_properties_t *aj_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *p = obs_properties_create();
	obs_properties_add_float_slider(p, "strength", "Kekuatan koreksi", 0.0, 1.0, 0.05);
	obs_properties_add_float_slider(p, "sharpness", "Ketajaman", 0.0, 1.0, 0.05);
	obs_properties_add_float_slider(p, "speed", "Kecepatan adaptasi", 0.0, 1.0, 0.05);
	obs_properties_add_bool(p, "auto_wb", "Warna otomatis (white balance)");
	obs_properties_add_bool(p, "auto_exp", "Kecerahan otomatis (exposure)");
	return p;
}

static void *aj_create(obs_data_t *settings, obs_source_t *source)
{
	struct aj_data *d = bzalloc(sizeof(struct aj_data));
	d->ctx = source;
	d->gain[0] = d->gain[1] = d->gain[2] = 1.0f;
	d->exposure = 1.0f;
	d->last_luma = AJ_TARGET_LUMA;

	char *path = obs_module_file("auto_color.effect");

	obs_enter_graphics();
	if (path)
		d->effect = gs_effect_create_from_file(path, NULL);
	d->render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	d->stage = gs_stagesurface_create(AJ_W, AJ_H, GS_RGBA);
	if (d->effect) {
		d->p_gain = gs_effect_get_param_by_name(d->effect, "gain");
		d->p_contrast = gs_effect_get_param_by_name(d->effect, "contrast");
		d->p_saturation = gs_effect_get_param_by_name(d->effect, "saturation");
		d->p_sharp = gs_effect_get_param_by_name(d->effect, "sharp");
		d->p_texel = gs_effect_get_param_by_name(d->effect, "texel");
	}
	obs_leave_graphics();

	if (!d->effect)
		blog(LOG_ERROR, "[AutoJernih] Gagal memuat auto_color.effect (path: %s)", path ? path : "?");
	bfree(path);

	aj_update(d, settings);
	return d;
}

static void aj_destroy(void *data)
{
	struct aj_data *d = data;
	obs_enter_graphics();
	if (d->effect)
		gs_effect_destroy(d->effect);
	if (d->render)
		gs_texrender_destroy(d->render);
	if (d->stage)
		gs_stagesurface_destroy(d->stage);
	obs_leave_graphics();
	bfree(d);
}

/* Hitung rata-rata warna dari versi kecil gambar, lalu geser nilai halus ke target. */
static void aj_analyze(struct aj_data *d, obs_source_t *target, uint32_t w, uint32_t h)
{
	gs_texrender_reset(d->render);
	if (!gs_texrender_begin(d->render, AJ_W, AJ_H))
		return;

	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	obs_source_video_render(target);
	gs_blend_state_pop();
	gs_texrender_end(d->render);

	gs_texture_t *tex = gs_texrender_get_texture(d->render);
	if (!tex)
		return;

	gs_stage_texture(d->stage, tex);

	uint8_t *pix = NULL;
	uint32_t linesize = 0;
	if (!gs_stagesurface_map(d->stage, &pix, &linesize))
		return;

	double sr = 0.0, sg = 0.0, sb = 0.0;
	for (uint32_t y = 0; y < AJ_H; y++) {
		const uint8_t *row = pix + (size_t)y * linesize;
		for (uint32_t x = 0; x < AJ_W; x++) {
			sr += row[x * 4 + 0];
			sg += row[x * 4 + 1];
			sb += row[x * 4 + 2];
		}
	}
	gs_stagesurface_unmap(d->stage);

	const double n = (double)(AJ_W * AJ_H);
	float mr = (float)(sr / n), mg = (float)(sg / n), mb = (float)(sb / n);
	float luma = 0.299f * mr + 0.587f * mg + 0.114f * mb;

	/* Abaikan frame hampir hitam (tidak ada sinyal) */
	if (luma < 8.0f)
		return;

	float gray = (mr + mg + mb) / 3.0f;
	float t_gain[3] = {1.0f, 1.0f, 1.0f};
	float t_exp = 1.0f;

	if (d->auto_wb) {
		/* "Gray world": rata-rata scene dianggap abu-abu netral. Dibatasi agar tidak ekstrem. */
		t_gain[0] = aj_clamp(gray / (mr > 1.0f ? mr : 1.0f), 0.80f, 1.25f);
		t_gain[1] = aj_clamp(gray / (mg > 1.0f ? mg : 1.0f), 0.80f, 1.25f);
		t_gain[2] = aj_clamp(gray / (mb > 1.0f ? mb : 1.0f), 0.80f, 1.25f);
	}
	if (d->auto_exp)
		t_exp = aj_clamp(AJ_TARGET_LUMA / luma, 0.75f, 1.35f);

	/* Kekuatan koreksi */
	for (int i = 0; i < 3; i++)
		t_gain[i] = 1.0f + d->strength * (t_gain[i] - 1.0f);
	t_exp = 1.0f + d->strength * (t_exp - 1.0f);

	/* Adaptasi halus; kalau kecerahan melompat (kamera berganti di mixer), adaptasi cepat. */
	float alpha = 0.03f + d->speed * 0.15f;
	if (fabsf(luma - d->last_luma) > 30.0f)
		alpha = 0.6f;
	d->last_luma = luma;

	for (int i = 0; i < 3; i++)
		d->gain[i] += (t_gain[i] - d->gain[i]) * alpha;
	d->exposure += (t_exp - d->exposure) * alpha;
}

static void aj_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct aj_data *d = data;

	obs_source_t *target = obs_filter_get_target(d->ctx);
	if (!d->effect || !target) {
		obs_source_skip_video_filter(d->ctx);
		return;
	}

	uint32_t w = obs_source_get_base_width(target);
	uint32_t h = obs_source_get_base_height(target);
	if (w == 0 || h == 0) {
		obs_source_skip_video_filter(d->ctx);
		return;
	}

	if ((d->auto_wb || d->auto_exp) && (d->frame++ % AJ_EVERY) == 0)
		aj_analyze(d, target, w, h);

	if (!obs_source_process_filter_begin(d->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING))
		return;

	struct vec3 gain;
	vec3_set(&gain, d->gain[0] * d->exposure, d->gain[1] * d->exposure, d->gain[2] * d->exposure);
	struct vec2 texel;
	vec2_set(&texel, 1.0f / (float)w, 1.0f / (float)h);

	gs_effect_set_vec3(d->p_gain, &gain);
	gs_effect_set_float(d->p_contrast, 1.0f + 0.12f * d->strength);
	gs_effect_set_float(d->p_saturation, 1.0f + 0.15f * d->strength);
	gs_effect_set_float(d->p_sharp, d->sharpness * 1.5f);
	gs_effect_set_vec2(d->p_texel, &texel);

	obs_source_process_filter_end(d->ctx, d->effect, w, h);
}

static struct obs_source_info aj_filter = {
	.id = "autojernih_auto_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = aj_get_name,
	.create = aj_create,
	.destroy = aj_destroy,
	.update = aj_update,
	.get_defaults = aj_defaults,
	.get_properties = aj_properties,
	.video_render = aj_render,
};

bool obs_module_load(void)
{
	obs_register_source(&aj_filter);
	blog(LOG_INFO, "[AutoJernih] dimuat");
	return true;
}
