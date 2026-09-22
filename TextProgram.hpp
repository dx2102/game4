#pragma once

#include "GL.hpp"
#include "Load.hpp"

//Shader program that draws glyph quads: vertex color, with alpha taken from a single-channel (GL_R8) atlas texture.
struct TextProgram {
	TextProgram();
	~TextProgram();

	GLuint program = 0;
	//Attribute (per-vertex variable) locations:
	GLuint Position_vec4 = -1U;
	GLuint TexCoord_vec2 = -1U;
	GLuint Color_vec4 = -1U;
	//Uniform (per-invocation variable) locations:
	GLuint CLIP_FROM_WORLD_mat4 = -1U;
	//Textures:
	//TEXTURE0 - glyph atlas (red channel = coverage)
};

extern Load< TextProgram > text_program;
