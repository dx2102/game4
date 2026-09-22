#pragma once

/*
 * TextRenderer -- shapes text with HarfBuzz, rasterizes glyphs with FreeType
 * into a single atlas texture, and draws batches of glyph quads with OpenGL.
 *
 * Designed around a *monospace variable font*:
 *  - every glyph gets an identical fixed-size cell in the atlas, so glyph
 *    placement (bearings, baseline) is resolved once when the cell is filled and
 *    never again at draw time;
 *  - the font's 'wght' axis is sampled at N evenly spaced weights; each weight
 *    is a separate band of cells, rasterized lazily the first time it is used.
 *
 * References I leaned on:
 *  - FreeType tutorial, steps 1-2: https://freetype.org/freetype2/docs/tutorial/step1.html
 *  - FreeType variable font API (ftmm.h): FT_Get_MM_Var / FT_Set_Var_Design_Coordinates
 *  - HarfBuzz "Getting started": https://harfbuzz.github.io/ch03s03.html
 *  - the 15-466 base code DrawLines.cpp / ColorTextureProgram.cpp for the GL side.
 */

#include "GL.hpp"

#include <glm/glm.hpp>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <hb.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct TextRenderer {
	//pixel_size: em size in physical pixels. weight_*: range of the 'wght' axis to sample.
	TextRenderer(std::string const &font_path, float pixel_size, float weight_min, float weight_max, float weight_step);
	~TextRenderer();
	TextRenderer(TextRenderer const &) = delete;
	TextRenderer &operator=(TextRenderer const &) = delete;

	//----- shaping (HarfBuzz) -----
	struct ShapedGlyph {
		uint32_t glyph;    //glyph index in the font
		uint32_t cluster;  //byte offset of the source character in the shaped string
		float advance;     //horizontal advance in pixels
	};
	std::vector< ShapedGlyph > shape(std::string_view text) const;

	//----- metrics (pixels) -----
	float ascender = 0.0f;    //baseline to top of line box (positive)
	float descender = 0.0f;   //baseline to bottom of line box (negative)
	float line_height = 0.0f; //recommended baseline-to-baseline distance
	float advance = 0.0f;     //advance of one monospace column

	//----- weights -----
	uint32_t weight_steps = 1;
	float weight_min = 400.0f, weight_step = 0.0f;
	//nearest sampled step for a design weight, clamped to the sampled range:
	uint32_t step_for_weight(float weight) const;

	//----- drawing -----
	//call begin(), then add() any number of glyphs, then end() to draw them all in one call:
	void begin();
	//pos is the pen position (left edge, on the baseline) in world units; world y points up.
	void add(uint32_t glyph, glm::vec2 const &pos, uint32_t weight_step, glm::u8vec4 const &color);
	void end(glm::mat4 const &clip_from_world);

	//----- internals -----
	struct Vertex {
		glm::vec2 Position;
		glm::vec2 TexCoord;
		glm::u8vec4 Color;
	};
	static_assert(sizeof(Vertex) == 2 * 4 + 2 * 4 + 4, "Vertex is packed");

private:
	FT_Library ft_library = nullptr;
	FT_Face ft_face = nullptr;
	std::vector< FT_Fixed > ft_default_coords; //design coordinates for all axes (16.16 fixed)
	int32_t wght_axis = -1; //index into ft_default_coords, or -1 if the font has no weight axis

	hb_blob_t *hb_blob = nullptr;
	hb_face_t *hb_face = nullptr;
	hb_font_t *hb_font = nullptr;

	//atlas layout:
	int32_t pad = 0;                       //empty border inside each cell (pixels)
	int32_t cell_w = 0, cell_h = 0;        //cell size (pixels)
	int32_t cell_top = 0;                  //baseline to top of cell (pixels, = ceil(ascender) + pad)
	uint32_t atlas_cols = 0, atlas_rows = 0;
	uint32_t atlas_w = 0, atlas_h = 0;
	std::vector< uint32_t > slot_glyphs;   //glyph index for each atlas slot (slot = column within a weight band)
	std::unordered_map< uint32_t, uint32_t > glyph_slots; //glyph index -> slot
	std::vector< bool > step_ready;        //has this weight band been rasterized yet?
	void rasterize_step(uint32_t step);

	GLuint atlas_tex = 0;
	GLuint vertex_buffer = 0;
	GLuint vertex_array = 0;
	std::vector< Vertex > vertices;
};
