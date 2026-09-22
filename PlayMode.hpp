#include "Mode.hpp"

#include "Story.hpp"
#include "TextRenderer.hpp"

#include <glm/glm.hpp>

#include <memory>
#include <string_view>
#include <vector>

/*
 * PlayMode -- runs the story: lays out the current node's text, shows its
 * choices, and turns keypresses into transitions.
 *
 * Controls: 1-4 pick a choice, R restarts.
 */
struct PlayMode : Mode {
	PlayMode();
	virtual ~PlayMode();

	//functions called by main loop:
	virtual bool handle_event(SDL_Event const &, glm::uvec2 const &window_size) override;
	virtual void update(float elapsed) override;
	virtual void draw(glm::uvec2 const &drawable_size) override;

	//----- text rendering -----
	std::unique_ptr< TextRenderer > text;
	float scale = 1.0f;   //physical pixels per logical pixel (2 on a retina display)
	float margin = 40.0f; //physical pixels

	//----- story state -----
	uint32_t node = 0;
	uint32_t flags = 0;
	int32_t minutes_left = 0;
	float time = 0.0f;      //seconds since start (drives the effects)
	int32_t held = -1;      //index into 'choices' whose key is currently held down, or -1
	SDL_Keycode held_key = 0;

	void restart();
	void enter_node(uint32_t index);
	void choose(uint32_t which);

	//----- layout of the current node (recomputed when the column count changes) -----
	struct GlyphInst {
		uint32_t glyph;
		uint32_t col, row;
		uint32_t order;      //index within the body (seeds the shake effect)
		uint8_t style;       //Story::StyleFlags
		glm::u8vec4 color;
	};
	std::vector< GlyphInst > body;
	uint32_t body_rows = 0;
	struct ChoiceView {
		uint32_t index; //index into the node's choices[]
		std::vector< TextRenderer::ShapedGlyph > label;
	};
	std::vector< ChoiceView > choices;
	uint32_t columns = 0;
	bool layout_dirty = true;
	void layout();

	//helpers used by draw():
	void draw_string(std::string_view s, glm::vec2 const &pos, glm::u8vec4 const &color, uint8_t style);
	void draw_glyph(uint32_t glyph, glm::vec2 const &pos, uint32_t col, uint8_t style, glm::u8vec4 const &color, uint32_t seed);
};
