#include "TextRenderer.hpp"

#include "TextProgram.hpp"
#include "gl_errors.hpp"

#include FT_MULTIPLE_MASTERS_H

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

TextRenderer::TextRenderer(std::string const &font_path, float pixel_size, float weight_min_, float weight_max, float weight_step_) {
	//----- FreeType: open the face and pick a pixel size -----
	if (FT_Init_FreeType(&ft_library)) throw std::runtime_error("FT_Init_FreeType failed");
	if (FT_New_Face(ft_library, font_path.c_str(), 0, &ft_face)) throw std::runtime_error("FT_New_Face failed for '" + font_path + "'");
	if (FT_Set_Pixel_Sizes(ft_face, 0, FT_UInt(std::lround(pixel_size)))) throw std::runtime_error("FT_Set_Pixel_Sizes failed");

	//size metrics are 26.6 fixed point (1/64 pixel):
	ascender = float(ft_face->size->metrics.ascender) / 64.0f;
	descender = float(ft_face->size->metrics.descender) / 64.0f;
	line_height = float(ft_face->size->metrics.height) / 64.0f;
	advance = float(ft_face->size->metrics.max_advance) / 64.0f;

	//----- FreeType: find the 'wght' variation axis, if any -----
	{
		FT_MM_Var *mm = nullptr;
		if (FT_Get_MM_Var(ft_face, &mm) == 0 && mm) {
			ft_default_coords.resize(mm->num_axis);
			for (FT_UInt a = 0; a < mm->num_axis; ++a) {
				ft_default_coords[a] = mm->axis[a].def;
				if (mm->axis[a].tag == FT_MAKE_TAG('w', 'g', 'h', 't')) {
					wght_axis = int32_t(a);
					//clamp requested range to what the font actually supports (16.16 -> float):
					float axis_min = float(mm->axis[a].minimum) / 65536.0f;
					float axis_max = float(mm->axis[a].maximum) / 65536.0f;
					weight_min_ = std::max(weight_min_, axis_min);
					weight_max = std::min(weight_max, axis_max);
				}
			}
			FT_Done_MM_Var(ft_library, mm);
		}
	}
	if (wght_axis >= 0 && weight_step_ > 0.0f && weight_max >= weight_min_) {
		weight_min = weight_min_;
		weight_step = weight_step_;
		weight_steps = uint32_t(std::floor((weight_max - weight_min) / weight_step + 1e-3f)) + 1;
	} else {
		//static font (or degenerate range): a single band at whatever the face's default weight is
		weight_min = 400.0f;
		weight_step = 0.0f;
		weight_steps = 1;
		if (wght_axis < 0) std::cout << "NOTE: '" << font_path << "' has no 'wght' axis; weight effects will be disabled." << std::endl;
	}
	step_ready.assign(weight_steps, false);

	//----- HarfBuzz: its own view of the same font file -----
	// (kept independent of the FreeType face so that changing FreeType's variation
	//  coordinates while rasterizing never affects shaping; a monospace font's
	//  advances don't change with weight anyway.)
	hb_blob = hb_blob_create_from_file(font_path.c_str());
	if (!hb_blob || hb_blob_get_length(hb_blob) == 0) throw std::runtime_error("hb_blob_create_from_file failed for '" + font_path + "'");
	hb_face = hb_face_create(hb_blob, 0);
	hb_font = hb_font_create(hb_face);
	//HarfBuzz reports positions in font units scaled by this; use 26.6 like FreeType so /64 gives pixels:
	int hb_scale = int(std::lround(pixel_size * 64.0f));
	hb_font_set_scale(hb_font, hb_scale, hb_scale);

	//----- atlas layout -----
	//one slot per printable ASCII glyph, plus slot 0 for .notdef (drawn for anything unmapped):
	slot_glyphs.push_back(0);
	glyph_slots[0] = 0;
	for (uint32_t c = 32; c < 127; ++c) {
		uint32_t g = FT_Get_Char_Index(ft_face, c);
		if (g == 0 || glyph_slots.count(g)) continue;
		glyph_slots[g] = uint32_t(slot_glyphs.size());
		slot_glyphs.push_back(g);
	}

	pad = 3;
	cell_w = int32_t(std::ceil(advance)) + 2 * pad;
	cell_h = int32_t(std::ceil(ascender) - std::floor(descender)) + 2 * pad;
	cell_top = int32_t(std::ceil(ascender)) + pad;

	uint32_t total_cells = uint32_t(slot_glyphs.size()) * weight_steps;
	//aim for a roughly square atlas:
	atlas_cols = std::max(1u, uint32_t(std::ceil(std::sqrt(double(total_cells) * cell_h / cell_w))));
	atlas_rows = (total_cells + atlas_cols - 1) / atlas_cols;
	atlas_w = atlas_cols * cell_w;
	atlas_h = atlas_rows * cell_h;
	GLint max_size = 0;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_size);
	if (atlas_w > uint32_t(max_size) || atlas_h > uint32_t(max_size)) {
		throw std::runtime_error("glyph atlas (" + std::to_string(atlas_w) + "x" + std::to_string(atlas_h) + ") exceeds GL_MAX_TEXTURE_SIZE");
	}

	std::cout << "TextRenderer: " << font_path << " @ " << pixel_size << "px; "
		<< slot_glyphs.size() << " glyphs x " << weight_steps << " weights; cell " << cell_w << "x" << cell_h
		<< "; atlas " << atlas_w << "x" << atlas_h << std::endl;

	//----- GL: atlas texture (single channel, starts fully transparent) -----
	glGenTextures(1, &atlas_tex);
	glBindTexture(GL_TEXTURE_2D, atlas_tex);
	{
		std::vector< uint8_t > zeros(size_t(atlas_w) * atlas_h, 0);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1); //rows of GL_RED data are not 4-byte aligned
		glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, atlas_w, atlas_h, 0, GL_RED, GL_UNSIGNED_BYTE, zeros.data());
	}
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);

	//----- GL: vertex buffer + vertex array for text_program -----
	// (same pattern as DrawLines.cpp)
	glGenBuffers(1, &vertex_buffer);
	glGenVertexArrays(1, &vertex_array);
	glBindVertexArray(vertex_array);
	glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
	glVertexAttribPointer(text_program->Position_vec4, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (GLbyte *)0 + offsetof(Vertex, Position));
	glEnableVertexAttribArray(text_program->Position_vec4);
	glVertexAttribPointer(text_program->TexCoord_vec2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (GLbyte *)0 + offsetof(Vertex, TexCoord));
	glEnableVertexAttribArray(text_program->TexCoord_vec2);
	glVertexAttribPointer(text_program->Color_vec4, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), (GLbyte *)0 + offsetof(Vertex, Color));
	glEnableVertexAttribArray(text_program->Color_vec4);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindVertexArray(0);

	GL_ERRORS();

	//the default weight is what most text uses, so rasterize it up front:
	rasterize_step(step_for_weight(400.0f));
}

TextRenderer::~TextRenderer() {
	glDeleteVertexArrays(1, &vertex_array);
	glDeleteBuffers(1, &vertex_buffer);
	glDeleteTextures(1, &atlas_tex);
	if (hb_font) hb_font_destroy(hb_font);
	if (hb_face) hb_face_destroy(hb_face);
	if (hb_blob) hb_blob_destroy(hb_blob);
	if (ft_face) FT_Done_Face(ft_face);
	if (ft_library) FT_Done_FreeType(ft_library);
}

std::vector< TextRenderer::ShapedGlyph > TextRenderer::shape(std::string_view text) const {
	hb_buffer_t *buf = hb_buffer_create();
	hb_buffer_add_utf8(buf, text.data(), int(text.size()), 0, int(text.size()));
	hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
	hb_buffer_set_script(buf, HB_SCRIPT_LATIN);
	hb_buffer_set_language(buf, hb_language_from_string("en", -1));

	//no features passed => the font's defaults (kern, calt, liga, ...) apply.
	// Cascadia Mono has no ASCII ligatures, so every input byte maps to one glyph.
	hb_shape(hb_font, buf, nullptr, 0);

	unsigned int count = hb_buffer_get_length(buf);
	hb_glyph_info_t *infos = hb_buffer_get_glyph_infos(buf, nullptr);
	hb_glyph_position_t *positions = hb_buffer_get_glyph_positions(buf, nullptr);

	std::vector< ShapedGlyph > ret;
	ret.reserve(count);
	for (unsigned int i = 0; i < count; ++i) {
		ret.push_back(ShapedGlyph{ infos[i].codepoint, infos[i].cluster, float(positions[i].x_advance) / 64.0f });
	}
	hb_buffer_destroy(buf);
	return ret;
}

uint32_t TextRenderer::step_for_weight(float weight) const {
	if (weight_steps <= 1) return 0;
	float s = std::round((weight - weight_min) / weight_step);
	return uint32_t(std::clamp(s, 0.0f, float(weight_steps - 1)));
}

void TextRenderer::rasterize_step(uint32_t step) {
	if (step >= weight_steps || step_ready[step]) return;
	step_ready[step] = true;

	if (wght_axis >= 0) {
		std::vector< FT_Fixed > coords = ft_default_coords;
		float weight = weight_min + weight_step * float(step);
		coords[wght_axis] = FT_Fixed(std::lround(weight * 65536.0f));
		if (FT_Set_Var_Design_Coordinates(ft_face, FT_UInt(coords.size()), coords.data())) {
			std::cerr << "WARNING: FT_Set_Var_Design_Coordinates failed for weight " << weight << std::endl;
		}
	}

	std::vector< uint8_t > cell(size_t(cell_w) * cell_h);
	bool warned = false;
	glBindTexture(GL_TEXTURE_2D, atlas_tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	for (uint32_t slot = 0; slot < slot_glyphs.size(); ++slot) {
		//no hinting: hinted outlines snap to the pixel grid differently at every weight, which makes animation jitter
		if (FT_Load_Glyph(ft_face, slot_glyphs[slot], FT_LOAD_NO_HINTING | FT_LOAD_RENDER)) {
			std::cerr << "WARNING: failed to render glyph " << slot_glyphs[slot] << std::endl;
			continue;
		}
		FT_Bitmap const &bm = ft_face->glyph->bitmap;
		std::fill(cell.begin(), cell.end(), uint8_t(0));

		//place the glyph bitmap inside the cell. cell rows are stored bottom-up (row 0 = bottom)
		// so that the texture's v axis and world y point the same way.
		int32_t x0 = pad + ft_face->glyph->bitmap_left;
		int32_t top_from_cell_top = cell_top - ft_face->glyph->bitmap_top; //rows from top of cell to top of bitmap
		for (uint32_t r = 0; r < bm.rows; ++r) {
			int32_t y_from_top = top_from_cell_top + int32_t(r);
			int32_t y = cell_h - 1 - y_from_top; //bottom-up row index
			for (uint32_t c = 0; c < bm.width; ++c) {
				int32_t x = x0 + int32_t(c);
				if (x < 0 || x >= cell_w || y < 0 || y >= cell_h) {
					if (!warned) {
						std::cerr << "WARNING: glyph " << slot_glyphs[slot] << " overflows its atlas cell; increase pad." << std::endl;
						warned = true;
					}
					continue;
				}
				cell[size_t(y) * cell_w + x] = bm.buffer[size_t(r) * size_t(bm.pitch) + c];
			}
		}

		uint32_t index = step * uint32_t(slot_glyphs.size()) + slot;
		uint32_t cx = (index % atlas_cols) * cell_w;
		uint32_t cy = (index / atlas_cols) * cell_h;
		glTexSubImage2D(GL_TEXTURE_2D, 0, cx, cy, cell_w, cell_h, GL_RED, GL_UNSIGNED_BYTE, cell.data());
	}
	glBindTexture(GL_TEXTURE_2D, 0);
	GL_ERRORS();
}

void TextRenderer::begin() {
	vertices.clear();
}

void TextRenderer::add(uint32_t glyph, glm::vec2 const &pos, uint32_t weight_step, glm::u8vec4 const &color) {
	if (weight_step >= weight_steps) weight_step = weight_steps - 1;
	rasterize_step(weight_step);

	auto f = glyph_slots.find(glyph);
	uint32_t slot = (f == glyph_slots.end() ? 0 : f->second);
	uint32_t index = weight_step * uint32_t(slot_glyphs.size()) + slot;
	float u0 = float((index % atlas_cols) * cell_w) / float(atlas_w);
	float v0 = float((index / atlas_cols) * cell_h) / float(atlas_h);
	float u1 = u0 + float(cell_w) / float(atlas_w);
	float v1 = v0 + float(cell_h) / float(atlas_h);

	//cell corners in world space: the cell's top row sits cell_top pixels above the baseline:
	float x0 = pos.x - float(pad);
	float x1 = x0 + float(cell_w);
	float y1 = pos.y + float(cell_top);
	float y0 = y1 - float(cell_h);

	//two triangles:
	vertices.emplace_back(Vertex{ glm::vec2(x0, y0), glm::vec2(u0, v0), color });
	vertices.emplace_back(Vertex{ glm::vec2(x1, y0), glm::vec2(u1, v0), color });
	vertices.emplace_back(Vertex{ glm::vec2(x1, y1), glm::vec2(u1, v1), color });
	vertices.emplace_back(Vertex{ glm::vec2(x0, y0), glm::vec2(u0, v0), color });
	vertices.emplace_back(Vertex{ glm::vec2(x1, y1), glm::vec2(u1, v1), color });
	vertices.emplace_back(Vertex{ glm::vec2(x0, y1), glm::vec2(u0, v1), color });
}

void TextRenderer::end(glm::mat4 const &clip_from_world) {
	if (vertices.empty()) return;

	glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
	glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(), GL_STREAM_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glUseProgram(text_program->program);
	glUniformMatrix4fv(text_program->CLIP_FROM_WORLD_mat4, 1, GL_FALSE, glm::value_ptr(clip_from_world));
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, atlas_tex);
	glBindVertexArray(vertex_array);

	glDrawArrays(GL_TRIANGLES, 0, GLsizei(vertices.size()));

	glBindVertexArray(0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glUseProgram(0);
	glDisable(GL_BLEND);

	GL_ERRORS();
}
