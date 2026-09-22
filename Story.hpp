#pragma once

/*
 * Story -- the branching narrative, loaded from dist/story.bin.
 *
 * story.bin is produced by compile_story.py from story.xml; see the header
 * comment of compile_story.py for the chunk layout. The structs below must
 * match the struct.pack formats used there byte-for-byte.
 */

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct Story {
	enum StyleFlags : uint8_t {
		Bold = 1,
		Pulse = 2,
		Shake = 4,
		Wave = 8,
		HasColor = 16, //if set, (r,g,b) override the default text color
	};

	struct StyleRun {
		uint32_t begin, end; //byte range within the node's text
		uint8_t flags;
		uint8_t r, g, b;
	};
	static_assert(sizeof(StyleRun) == 12, "StyleRun is packed");

	struct Choice {
		uint32_t label_begin, label_end; //range in 'strings'
		uint32_t target; //node index
		uint32_t require; //flags that must all be set for the choice to show
		uint32_t forbid;  //flags that must all be clear for the choice to show
		uint32_t set;     //flags set when the choice is taken
		uint32_t cost;    //minutes taken off the clock
	};
	static_assert(sizeof(Choice) == 28, "Choice is packed");

	enum : uint32_t { MaxChoices = 4 };

	struct Node {
		uint32_t name_begin, name_end;   //id string (for debugging)
		uint32_t form_begin, form_end;   //what you currently are ("hat", "mop", ...)
		uint32_t text_begin, text_end;   //body text; paragraphs separated by '\n'
		uint32_t style_begin, style_end; //range in 'styles'
		uint32_t set_flags;  //flags set on entering this node
		uint32_t is_ending;  //1 if this node ends the game
		uint32_t choice_count;
		Choice choices[MaxChoices];
	};
	static_assert(sizeof(Node) == 44 + 28 * MaxChoices, "Node is packed");

	uint32_t start = 0;   //index of first node
	uint32_t timeout = 0; //node jumped to when the clock runs out
	uint32_t clock = 10;  //starting minutes

	std::string strings;
	std::vector< Node > nodes;
	std::vector< StyleRun > styles;

	//throws on read error:
	Story(std::string const &filename);

	std::string_view str(uint32_t begin, uint32_t end) const {
		return std::string_view(strings).substr(begin, end - begin);
	}
	std::string_view name(Node const &n) const { return str(n.name_begin, n.name_end); }
	std::string_view form(Node const &n) const { return str(n.form_begin, n.form_end); }
	std::string_view text(Node const &n) const { return str(n.text_begin, n.text_end); }
	std::string_view label(Choice const &c) const { return str(c.label_begin, c.label_end); }
};
