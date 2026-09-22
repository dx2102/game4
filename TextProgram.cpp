#include "TextProgram.hpp"

#include "gl_compile_program.hpp"
#include "gl_errors.hpp"

Load< TextProgram > text_program(LoadTagEarly);

TextProgram::TextProgram() {
	//based on ColorTextureProgram.cpp; the only real difference is the fragment shader,
	// which treats the (single-channel) texture as coverage/alpha instead of as a color:
	program = gl_compile_program(
		//vertex shader:
		"#version 330\n"
		"uniform mat4 CLIP_FROM_WORLD;\n"
		"in vec4 Position;\n"
		"in vec2 TexCoord;\n"
		"in vec4 Color;\n"
		"out vec2 texCoord;\n"
		"out vec4 color;\n"
		"void main() {\n"
		"	gl_Position = CLIP_FROM_WORLD * Position;\n"
		"	texCoord = TexCoord;\n"
		"	color = Color;\n"
		"}\n"
	,
		//fragment shader:
		"#version 330\n"
		"uniform sampler2D TEX;\n"
		"in vec2 texCoord;\n"
		"in vec4 color;\n"
		"out vec4 fragColor;\n"
		"void main() {\n"
		"	float coverage = texture(TEX, texCoord).r;\n"
		//vertex colors are authored as sRGB values; the framebuffer is sRGB-encoded (GL_FRAMEBUFFER_SRGB in main.cpp),
		// so convert to linear here or everything comes out washed out:
		"	vec3 linear = pow(color.rgb, vec3(2.2));\n"
		"	fragColor = vec4(linear, color.a * coverage);\n"
		"}\n"
	);

	//look up the locations of vertex attributes:
	Position_vec4 = glGetAttribLocation(program, "Position");
	TexCoord_vec2 = glGetAttribLocation(program, "TexCoord");
	Color_vec4 = glGetAttribLocation(program, "Color");

	//look up the locations of uniforms:
	CLIP_FROM_WORLD_mat4 = glGetUniformLocation(program, "CLIP_FROM_WORLD");
	GLuint TEX_sampler2D = glGetUniformLocation(program, "TEX");

	//set TEX to always refer to texture binding zero:
	glUseProgram(program);
	glUniform1i(TEX_sampler2D, 0);
	glUseProgram(0);

	GL_ERRORS();
}

TextProgram::~TextProgram() {
	glDeleteProgram(program);
	program = 0;
}
