#include "Story.hpp"

#include "read_write_chunk.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>

Story::Story(std::string const &filename) {
	std::ifstream file(filename, std::ios::binary);
	if (!file) throw std::runtime_error("Failed to open story file '" + filename + "'.");

	struct Header { uint32_t start, timeout, clock; };
	std::vector< Header > header;
	read_chunk(file, "hdr0", &header);
	if (header.size() != 1) throw std::runtime_error("story header chunk should hold exactly one header");
	start = header[0].start;
	timeout = header[0].timeout;
	clock = header[0].clock;

	std::vector< char > pool;
	read_chunk(file, "str0", &pool);
	strings.assign(pool.begin(), pool.end());

	read_chunk(file, "node", &nodes);
	read_chunk(file, "styl", &styles);

	//sanity-check every range so bad data fails here instead of somewhere in drawing code:
	auto check_range = [&](uint32_t begin, uint32_t end, size_t limit, char const *what) {
		if (begin > end || end > limit) throw std::runtime_error(std::string("story: bad ") + what + " range");
	};
	if (start >= nodes.size() || timeout >= nodes.size()) throw std::runtime_error("story: start/timeout node out of range");
	for (auto const &n : nodes) {
		check_range(n.name_begin, n.name_end, strings.size(), "name");
		check_range(n.form_begin, n.form_end, strings.size(), "form");
		check_range(n.text_begin, n.text_end, strings.size(), "text");
		check_range(n.style_begin, n.style_end, styles.size(), "style");
		if (n.choice_count > MaxChoices) throw std::runtime_error("story: too many choices");
		for (uint32_t i = 0; i < n.choice_count; ++i) {
			check_range(n.choices[i].label_begin, n.choices[i].label_end, strings.size(), "label");
			if (n.choices[i].target >= nodes.size()) throw std::runtime_error("story: choice target out of range");
		}
		for (uint32_t s = n.style_begin; s < n.style_end; ++s) {
			check_range(styles[s].begin, styles[s].end, n.text_end - n.text_begin, "style run");
		}
	}

	std::cout << "Loaded story '" << filename << "': " << nodes.size() << " nodes, " << styles.size() << " style runs." << std::endl;
}
