#include "PlayMode.hpp"

#include "Load.hpp"
#include "data_path.hpp"
#include "gl_errors.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iostream>
#include <string>

//----- tunables -----
constexpr float Tau = 6.28318530718f;
constexpr float FontSizeLogical = 22.0f;   //em size in logical pixels
constexpr float WeightMin = 200.0f, WeightMax = 700.0f, WeightStep = 10.0f; //sampled band of the 'wght' axis (Cascadia Mono's full range)
constexpr float WeightBody = 350.0f;
constexpr float WeightBold = 620.0f;
constexpr float WaveColumns = 20.0f;       //wavelength of the pulse and wave effects, in columns
constexpr float WaveSpeed = 12.0f;         //how fast the crests travel, in columns per second
constexpr float PhasePerCol = Tau / WaveColumns;          //phase step per column (the waves travel left-to-right)
constexpr float WaveOmega = Tau * WaveSpeed / WaveColumns; //temporal angular frequency (0.6 Hz)

//----- colors -----
constexpr glm::u8vec4 ColorBody(215, 215, 205, 255);
constexpr glm::u8vec4 ColorDim(120, 120, 115, 255);
constexpr glm::u8vec4 ColorChoice(150, 215, 225, 255);
constexpr glm::u8vec4 ColorClock(222, 205, 160, 255);
constexpr glm::u8vec4 ColorRed(235, 90, 80, 255);

//color of the header "YOU ARE ..." line, by form name (matches the palette in compile_story.py):
static glm::u8vec4 form_color(std::string_view form) {
	if (form == "hat") return glm::u8vec4(240, 200, 90, 255);
	if (form == "mop") return glm::u8vec4(200, 150, 100, 255);
	if (form == "vending machine") return glm::u8vec4(120, 220, 150, 255);
	if (form == "guard") return glm::u8vec4(120, 170, 240, 255);
	if (form == "pigeon") return glm::u8vec4(185, 175, 205, 255);
	return ColorBody;
}

//cheap integer hash -> [0,1), used for the shake effect:
static float hash01(uint32_t a, uint32_t b) {
	uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
	h ^= h >> 15; h *= 0xC2B2AE3Du; h ^= h >> 13;
	return float(h & 0xFFFFFFu) / 16777216.0f;
}

Load< Story > story(LoadTagDefault, []() -> Story const * {
	return new Story(data_path("story.bin"));
});

PlayMode::PlayMode() {
	//physical pixels per logical pixel (differs from 1 on high-DPI displays):
	{
		int w = 1, h = 1, pw = 1, ph = 1;
		SDL_GetWindowSize(Mode::window, &w, &h);
		SDL_GetWindowSizeInPixels(Mode::window, &pw, &ph);
		scale = (h > 0 ? float(ph) / float(h) : 1.0f);
	}
	margin = 40.0f * scale;
	text = std::make_unique< TextRenderer >(data_path("CascadiaMono.ttf"), FontSizeLogical * scale, WeightMin, WeightMax, WeightStep);

	restart();
}

PlayMode::~PlayMode() {
}

//----- story logic -----

void PlayMode::restart() {
	flags = 0;
	minutes_left = int32_t(story->clock);
	enter_node(story->start);
}

void PlayMode::enter_node(uint32_t index) {
	node = index;
	flags |= story->nodes[node].set_flags;
	held = -1;
	layout_dirty = true;
}

void PlayMode::choose(uint32_t which) {
	if (which >= choices.size()) return;
	Story::Node const &n = story->nodes[node];
	Story::Choice const &c = n.choices[choices[which].index];
	flags |= c.set;
	minutes_left -= int32_t(c.cost);
	uint32_t target = c.target;
	//out of time? every transition that isn't already an ending becomes the timeout ending:
	if (minutes_left <= 0 && !story->nodes[target].is_ending) target = story->timeout;
	enter_node(target);
}

//----- layout -----

void PlayMode::layout() {
	layout_dirty = false;
	body.clear();
	choices.clear();
	Story::Node const &n = story->nodes[node];
	std::string_view txt = story->text(n);

	//style lookup for a byte offset into the node text:
	auto style_at = [&](uint32_t offset, uint8_t *flags_out, glm::u8vec4 *color_out) {
		*flags_out = 0;
		*color_out = ColorBody;
		for (uint32_t s = n.style_begin; s < n.style_end; ++s) {
			Story::StyleRun const &run = story->styles[s];
			if (offset >= run.begin && offset < run.end) {
				*flags_out = run.flags;
				if (run.flags & Story::HasColor) *color_out = glm::u8vec4(run.r, run.g, run.b, 255);
				return;
			}
		}
	};

	//word-wrap: paragraphs are separated by '\n'; words by ' '. Monospace => one column per glyph.
	uint32_t cols = std::max(columns, 1u);
	uint32_t row = 0, col = 0, order = 0;
	size_t pos = 0;
	while (pos <= txt.size()) {
		size_t nl = txt.find('\n', pos);
		if (nl == std::string_view::npos) nl = txt.size();
		std::string_view para = txt.substr(pos, nl - pos);
		size_t wpos = 0;
		col = 0;
		while (wpos < para.size()) {
			size_t wend = para.find(' ', wpos);
			if (wend == std::string_view::npos) wend = para.size();
			std::string_view word = para.substr(wpos, wend - wpos);
			if (!word.empty()) {
				std::vector< TextRenderer::ShapedGlyph > glyphs = text->shape(word);
				//wrap if the word doesn't fit on this line (unless the line is empty -- then it gets hard-split):
				if (col > 0 && col + glyphs.size() > cols) { row += 1; col = 0; }
				for (auto const &g : glyphs) {
					if (col >= cols) { row += 1; col = 0; } //hard split for words longer than a line
					uint32_t offset = uint32_t(pos + wpos + g.cluster);
					GlyphInst inst;
					inst.glyph = g.glyph;
					inst.col = col;
					inst.row = row;
					inst.order = order++;
					style_at(offset, &inst.style, &inst.color);
					body.push_back(inst);
					col += 1;
				}
				col += 1; //the space after the word
			}
			wpos = wend + 1;
		}
		row += 2; //blank line between paragraphs
		pos = nl + 1;
	}
	body_rows = row;

	//choices that are currently available:
	for (uint32_t i = 0; i < n.choice_count; ++i) {
		Story::Choice const &c = n.choices[i];
		if ((flags & c.require) != c.require) continue;
		if ((flags & c.forbid) != 0) continue;
		choices.push_back(ChoiceView{ i, text->shape(story->label(c)) });
	}
}

//----- input -----

bool PlayMode::handle_event(SDL_Event const &evt, glm::uvec2 const &window_size) {
	if (evt.type != SDL_EVENT_KEY_DOWN && evt.type != SDL_EVENT_KEY_UP) return false;
	SDL_Keycode key = evt.key.key;

	if (evt.type == SDL_EVENT_KEY_DOWN && key == SDLK_R) {
		restart();
		return true;
	}

	//which choice does this key name? (-1 if none)
	int32_t which = -1;
	if (key >= SDLK_1 && key <= SDLK_4) which = int32_t(key - SDLK_1);
	else if (key >= SDLK_KP_1 && key <= SDLK_KP_4) which = int32_t(key - SDLK_KP_1);
	if (which < 0 || uint32_t(which) >= choices.size()) return false;

	//key down highlights the choice (bold); key up takes it, immediately:
	if (evt.type == SDL_EVENT_KEY_DOWN) {
		held = which;
		held_key = key;
	} else if (held == which && key == held_key) {
		held = -1;
		choose(uint32_t(which));
	}
	return true;
}

void PlayMode::update(float elapsed) {
	time += elapsed;
}

//----- drawing -----

void PlayMode::draw_glyph(uint32_t glyph, glm::vec2 const &pos, uint32_t col, uint8_t style, glm::u8vec4 const &color, uint32_t seed) {
	float weight = WeightBody;
	if (style & Story::Bold) weight = WeightBold;
	//a weight wave travelling left-to-right (phase decreases with column),
	// swinging across the font's whole range (200-700):
	if (style & Story::Pulse) weight = 450.0f + 250.0f * std::sin(WaveOmega * time - PhasePerCol * float(col));

	glm::vec2 p = pos;
	if (style & Story::Shake) {
		uint32_t frame = uint32_t(time * 20.0f); //re-roll the jitter 20 times a second
		p.x += (hash01(seed * 2 + 0, frame) - 0.5f) * 3.0f * scale;
		p.y += (hash01(seed * 2 + 1, frame) - 0.5f) * 3.0f * scale;
	}
	if (style & Story::Wave) {
		p.y += 2.0f * scale * std::sin(WaveOmega * time - PhasePerCol * float(col)); //same speed and wavelength as the pulse
	}
	text->add(glyph, p, text->step_for_weight(weight), color);
}

void PlayMode::draw_string(std::string_view s, glm::vec2 const &pos, glm::u8vec4 const &color, uint8_t style) {
	std::vector< TextRenderer::ShapedGlyph > glyphs = text->shape(s);
	float x = pos.x;
	uint32_t col = 0;
	for (auto const &g : glyphs) {
		draw_glyph(g.glyph, glm::vec2(x, pos.y), col, style, color, 100000 + col);
		x += g.advance;
		col += 1;
	}
}

void PlayMode::draw(glm::uvec2 const &drawable_size) {
	//columns available for body text; re-layout when the window width changes:
	uint32_t cols = uint32_t(std::max(1.0f, std::floor((float(drawable_size.x) - 2.0f * margin) / text->advance)));
	if (cols != columns) { columns = cols; layout_dirty = true; }
	if (layout_dirty) layout();

	//(linear values; the sRGB framebuffer displays this as roughly #18181c)
	glClearColor(0.0075f, 0.0075f, 0.0095f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glDisable(GL_DEPTH_TEST);

	Story::Node const &n = story->nodes[node];
	float const W = float(drawable_size.x);
	float const H = float(drawable_size.y);
	float const top_baseline = H - margin - text->ascender; //baseline of row 0
	auto row_baseline = [&](uint32_t row) { return top_baseline - float(row) * text->line_height; };

	text->begin();

	//--- header: what you are (left) and the clock (right) ---
	{
		std::string const prefix = "YOU ARE THE ";
		std::string who;
		for (char c : story->form(n)) who += char(std::toupper(static_cast< unsigned char >(c)));
		draw_string(prefix, glm::vec2(margin, row_baseline(0)), ColorBody, Story::Bold);
		draw_string(who, glm::vec2(margin + float(prefix.size()) * text->advance, row_baseline(0)), form_color(story->form(n)), Story::Bold);

		std::string clock;
		glm::u8vec4 clock_color = ColorClock;
		if (n.is_ending) {
			clock = "23:59";
			clock_color = ColorDim;
		} else {
			clock = "LAST TRAIN IN " + std::to_string(std::max(minutes_left, 0)) + " MIN";
			if (minutes_left <= 3) clock_color = ColorRed;
		}
		float x = W - margin - float(clock.size()) * text->advance;
		draw_string(clock, glm::vec2(x, row_baseline(0)), clock_color, 0);
	}

	//--- body text ---
	{
		constexpr uint32_t BodyRow = 2;
		for (auto const &g : body) {
			glm::vec2 pos(margin + float(g.col) * text->advance, row_baseline(BodyRow + g.row));
			draw_glyph(g.glyph, pos, g.col, g.style, g.color, g.order);
		}
	}

	//--- choices, anchored to the bottom ---
	{
		float bottom_baseline = margin - text->descender;
		if (n.is_ending) {
			draw_string("[ R ]  start over", glm::vec2(margin, bottom_baseline), ColorDim, 0);
		} else {
			uint32_t count = uint32_t(choices.size());
			for (uint32_t i = 0; i < count; ++i) {
				float y = bottom_baseline + float(count - 1 - i) * text->line_height;
				bool is_held = (held == int32_t(i)); //key is down: show it bold
				std::string number = "[ " + std::to_string(i + 1) + " ]  ";
				draw_string(number, glm::vec2(margin, y), is_held ? ColorBody : ColorDim, is_held ? Story::Bold : 0);
				float x = margin + float(number.size()) * text->advance;
				uint32_t col = 0;
				for (auto const &g : choices[i].label) {
					draw_glyph(g.glyph, glm::vec2(x, y), col, is_held ? Story::Bold : 0, ColorChoice, 200000 + i * 1000 + col);
					x += g.advance;
					col += 1;
				}
			}
		}
	}

	//pixel-space orthographic projection, origin at the bottom-left:
	glm::mat4 clip_from_world = glm::ortho(0.0f, W, 0.0f, H, -1.0f, 1.0f);
	text->end(clip_from_world);

	GL_ERRORS();
}
