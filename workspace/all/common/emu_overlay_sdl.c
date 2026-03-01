/*
 * emu_overlay_sdl.c — SDL2 + SDL_ttf render backend for the emulator overlay.
 *
 * This is a GL<->SDL bridge: menu elements are drawn onto an SDL_Surface using
 * SDL_ttf for text, then the composited surface is uploaded as a GL texture and
 * drawn as a fullscreen quad inside the emulator's GL context.
 *
 * CRITICAL: All GL state must be saved before overlay rendering and restored
 * after, or GLideN64's cached state tracking breaks (black screen on PowerVR).
 */

#include "emu_overlay_sdl.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_image.h>
#include <GLES3/gl3.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Scale factor & font sizes (matching NextUI's defines.h)
// ---------------------------------------------------------------------------

static int s_scale = 2;

#define FONT_SIZE_LARGE (16 * s_scale)
#define FONT_SIZE_SMALL (12 * s_scale)
#define FONT_SIZE_TINY (10 * s_scale)

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static int s_screenW = 0;
static int s_screenH = 0;

static TTF_Font* s_fonts[3] = {NULL, NULL, NULL}; // LARGE, SMALL, TINY

static SDL_Surface* s_renderSurface = NULL;	 // ARGB8888 compositing surface
static SDL_Surface* s_captureSurface = NULL; // Captured game frame (ARGB8888)

static GLuint s_overlayTexture = 0; // GL texture for uploading composited surface
static GLuint s_texProgram = 0;
static GLint s_texLocTexture = -1;
static GLuint s_texVAO = 0;
static GLuint s_texVBO = 0;

// Pixel conversion buffer (ARGB -> RGBA for GL upload)
static uint8_t* s_uploadBuffer = NULL;

// Icon storage (PNG images for button hints + slot screenshots)
#define MAX_ICONS 16
static SDL_Surface* s_icons[MAX_ICONS];
static int s_iconCount = 0;

// Saved GL state for begin_frame / end_frame
static GLint s_savedViewport[4];
static GLint s_savedScissorBox[4];
static GLboolean s_savedBlend;
static GLboolean s_savedDepthTest;
static GLboolean s_savedCullFace;
static GLboolean s_savedScissorTest;
static GLint s_savedBlendSrcRGB;
static GLint s_savedBlendDstRGB;
static GLint s_savedBlendSrcAlpha;
static GLint s_savedBlendDstAlpha;
static GLint s_savedProgram;
static GLint s_savedVAO;
static GLint s_savedVBO;
static GLint s_savedTex0;
static GLint s_savedActiveTexUnit;
static GLint s_savedUnpackAlignment;

// ---------------------------------------------------------------------------
// Shader sources
// ---------------------------------------------------------------------------

static const char* s_texVS =
	"#version 300 es\n"
	"in vec2 aPos;\n"
	"in vec2 aTexCoord;\n"
	"out vec2 vTexCoord;\n"
	"void main() {\n"
	"    gl_Position = vec4(aPos, 0.0, 1.0);\n"
	"    vTexCoord = aTexCoord;\n"
	"}\n";

static const char* s_texFS =
	"#version 300 es\n"
	"precision mediump float;\n"
	"in vec2 vTexCoord;\n"
	"uniform sampler2D uTexture;\n"
	"out vec4 fragColor;\n"
	"void main() {\n"
	"    fragColor = texture(uTexture, vTexCoord);\n"
	"}\n";

// ---------------------------------------------------------------------------
// GL helpers
// ---------------------------------------------------------------------------

static GLuint compile_shader(GLenum type, const char* src) {
	GLuint sh = glCreateShader(type);
	if (sh == 0) {
		fprintf(stderr, "[OverlaySDL] glCreateShader failed, glError=%d\n", glGetError());
		return 0;
	}
	glShaderSource(sh, 1, &src, NULL);
	glCompileShader(sh);

	GLint ok = 0;
	glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		log[0] = '\0';
		GLsizei logLen = 0;
		glGetShaderInfoLog(sh, (GLsizei)(sizeof(log) - 1), &logLen, log);
		log[logLen] = '\0';
		fprintf(stderr, "[OverlaySDL] shader compile error (%s): %s\n",
				type == GL_VERTEX_SHADER ? "VS" : "FS", log);
		glDeleteShader(sh);
		return 0;
	}
	return sh;
}

static GLuint link_program(GLuint vs, GLuint fs) {
	GLuint prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glLinkProgram(prog);

	GLint ok = 0;
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetProgramInfoLog(prog, (GLsizei)sizeof(log), NULL, log);
		fprintf(stderr, "[OverlaySDL] program link error: %s\n", log);
		glDeleteProgram(prog);
		return 0;
	}
	return prog;
}

// ---------------------------------------------------------------------------
// init / destroy
// ---------------------------------------------------------------------------

static int ovl_sdl_init(int screen_w, int screen_h) {
	s_screenW = screen_w;
	s_screenH = screen_h;

	// Scale factor: match NextUI's FIXED_SCALE
	// Brick (1024x768) = 3x, Smart Pro / TG5050 (1280x720) = 2x
	if (screen_w <= 1024)
		s_scale = 3;
	else
		s_scale = 2;

	// Initialize SDL_ttf
	if (!TTF_WasInit()) {
		if (TTF_Init() < 0) {
			fprintf(stderr, "[OverlaySDL] TTF_Init failed: %s\n", TTF_GetError());
			return -1;
		}
	}

	// Initialize SDL_image for PNG icon loading
	if (!(IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG)) {
		fprintf(stderr, "[OverlaySDL] IMG_Init(PNG) failed: %s\n", IMG_GetError());
		// Non-fatal: icons will just not load
	}

	// Reset icon storage
	for (int i = 0; i < MAX_ICONS; i++)
		s_icons[i] = NULL;
	s_iconCount = 0;

	// Load font
	const char* font_path = getenv("EMU_OVERLAY_FONT");
	if (!font_path || font_path[0] == '\0') {
		fprintf(stderr, "[OverlaySDL] EMU_OVERLAY_FONT not set\n");
		return -1;
	}

	int font_sizes[3] = {FONT_SIZE_LARGE, FONT_SIZE_SMALL, FONT_SIZE_TINY};
	for (int i = 0; i < 3; i++) {
		s_fonts[i] = TTF_OpenFont(font_path, font_sizes[i]);
		if (!s_fonts[i]) {
			fprintf(stderr, "[OverlaySDL] TTF_OpenFont(%s, %d) failed: %s\n",
					font_path, font_sizes[i], TTF_GetError());
			// Clean up already-opened fonts
			for (int j = 0; j < i; j++) {
				TTF_CloseFont(s_fonts[j]);
				s_fonts[j] = NULL;
			}
			return -1;
		}
	}

	// Create render surface (ARGB8888)
	s_renderSurface = SDL_CreateRGBSurfaceWithFormat(
		0, screen_w, screen_h, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!s_renderSurface) {
		fprintf(stderr, "[OverlaySDL] SDL_CreateRGBSurfaceWithFormat failed: %s\n",
				SDL_GetError());
		return -1;
	}

	// Create capture surface (ARGB8888)
	s_captureSurface = SDL_CreateRGBSurfaceWithFormat(
		0, screen_w, screen_h, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!s_captureSurface) {
		fprintf(stderr, "[OverlaySDL] SDL_CreateRGBSurfaceWithFormat (capture) failed: %s\n",
				SDL_GetError());
		return -1;
	}

	// Allocate upload buffer for ARGB -> RGBA conversion
	s_uploadBuffer = (uint8_t*)malloc((size_t)screen_w * (size_t)screen_h * 4);
	if (!s_uploadBuffer) {
		fprintf(stderr, "[OverlaySDL] malloc upload buffer failed\n");
		return -1;
	}

	// --- GL setup ---
	// Save GL state during init (same as OverlayGL.cpp pattern)
	GLint savedVAO = 0, savedVBO = 0, savedTex = 0, savedUnpackAlign = 4;
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &savedVAO);
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &savedVBO);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex);
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &savedUnpackAlign);

	// Build textured fullscreen quad shader
	{
		GLuint vs = compile_shader(GL_VERTEX_SHADER, s_texVS);
		GLuint fs = compile_shader(GL_FRAGMENT_SHADER, s_texFS);
		if (!vs || !fs) {
			if (vs)
				glDeleteShader(vs);
			if (fs)
				glDeleteShader(fs);
			// Restore GL state before returning
			glBindVertexArray(savedVAO);
			glBindBuffer(GL_ARRAY_BUFFER, savedVBO);
			glBindTexture(GL_TEXTURE_2D, savedTex);
			glPixelStorei(GL_UNPACK_ALIGNMENT, savedUnpackAlign);
			return -1;
		}
		s_texProgram = link_program(vs, fs);
		glDeleteShader(vs);
		glDeleteShader(fs);
		if (!s_texProgram) {
			glBindVertexArray(savedVAO);
			glBindBuffer(GL_ARRAY_BUFFER, savedVBO);
			glBindTexture(GL_TEXTURE_2D, savedTex);
			glPixelStorei(GL_UNPACK_ALIGNMENT, savedUnpackAlign);
			return -1;
		}
		s_texLocTexture = glGetUniformLocation(s_texProgram, "uTexture");
	}

	// VAO/VBO for textured fullscreen quad (6 verts * 4 floats: pos.xy + uv.xy)
	glGenVertexArrays(1, &s_texVAO);
	glGenBuffers(1, &s_texVBO);
	glBindVertexArray(s_texVAO);
	glBindBuffer(GL_ARRAY_BUFFER, s_texVBO);
	glBufferData(GL_ARRAY_BUFFER, 6 * 4 * (GLsizeiptr)sizeof(float), NULL, GL_DYNAMIC_DRAW);
	GLint posLoc = glGetAttribLocation(s_texProgram, "aPos");
	GLint uvLoc = glGetAttribLocation(s_texProgram, "aTexCoord");
	glEnableVertexAttribArray(posLoc);
	glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 4 * (GLsizei)sizeof(float), (void*)0);
	glEnableVertexAttribArray(uvLoc);
	glVertexAttribPointer(uvLoc, 2, GL_FLOAT, GL_FALSE, 4 * (GLsizei)sizeof(float),
						  (void*)(2 * sizeof(float)));
	glBindVertexArray(0);

	// Overlay texture
	glGenTextures(1, &s_overlayTexture);
	glBindTexture(GL_TEXTURE_2D, s_overlayTexture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);

	// Restore GL state
	glBindVertexArray(savedVAO);
	glBindBuffer(GL_ARRAY_BUFFER, savedVBO);
	glBindTexture(GL_TEXTURE_2D, savedTex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, savedUnpackAlign);

	return 0;
}

static void ovl_sdl_destroy(void) {
	for (int i = 0; i < 3; i++) {
		if (s_fonts[i]) {
			TTF_CloseFont(s_fonts[i]);
			s_fonts[i] = NULL;
		}
	}

	// Free loaded icons
	for (int i = 0; i < s_iconCount; i++) {
		if (s_icons[i]) {
			SDL_FreeSurface(s_icons[i]);
			s_icons[i] = NULL;
		}
	}
	s_iconCount = 0;
	IMG_Quit();

	if (s_renderSurface) {
		SDL_FreeSurface(s_renderSurface);
		s_renderSurface = NULL;
	}
	if (s_captureSurface) {
		SDL_FreeSurface(s_captureSurface);
		s_captureSurface = NULL;
	}

	free(s_uploadBuffer);
	s_uploadBuffer = NULL;

	if (s_texProgram) {
		glDeleteProgram(s_texProgram);
		s_texProgram = 0;
	}
	if (s_texVAO) {
		glDeleteVertexArrays(1, &s_texVAO);
		s_texVAO = 0;
	}
	if (s_texVBO) {
		glDeleteBuffers(1, &s_texVBO);
		s_texVBO = 0;
	}
	if (s_overlayTexture) {
		glDeleteTextures(1, &s_overlayTexture);
		s_overlayTexture = 0;
	}
}

// ---------------------------------------------------------------------------
// capture_frame — grab current GL framebuffer into SDL surface
// ---------------------------------------------------------------------------

static void ovl_sdl_capture_frame(void) {
	if (!s_captureSurface)
		return;

	int w = s_screenW;
	int h = s_screenH;

	// Temporary buffer for glReadPixels (RGBA, bottom-up)
	size_t row_bytes = (size_t)w * 4;
	uint8_t* gl_pixels = (uint8_t*)malloc(row_bytes * (size_t)h);
	if (!gl_pixels)
		return;

	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, gl_pixels);

	// Copy into capture surface, flipping vertically and converting RGBA -> ARGB
	SDL_LockSurface(s_captureSurface);
	uint8_t* dst_base = (uint8_t*)s_captureSurface->pixels;
	int dst_pitch = s_captureSurface->pitch;

	for (int y = 0; y < h; y++) {
		// GL row 0 is bottom, SDL row 0 is top => flip
		const uint8_t* src_row = gl_pixels + (size_t)(h - 1 - y) * row_bytes;
		uint8_t* dst_row = dst_base + (size_t)y * (size_t)dst_pitch;

		for (int x = 0; x < w; x++) {
			uint8_t r = src_row[x * 4 + 0];
			uint8_t g = src_row[x * 4 + 1];
			uint8_t b = src_row[x * 4 + 2];
			uint8_t a = src_row[x * 4 + 3];
			// ARGB8888: byte order in memory is B, G, R, A on little-endian
			// SDL_PIXELFORMAT_ARGB8888 stores as uint32 = (A<<24)|(R<<16)|(G<<8)|B
			uint32_t* dst_px = (uint32_t*)(dst_row + x * 4);
			*dst_px = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
					  ((uint32_t)g << 8) | (uint32_t)b;
		}
	}

	SDL_UnlockSurface(s_captureSurface);
	free(gl_pixels);
}

// ---------------------------------------------------------------------------
// draw_captured_frame — blit captured frame with dimming
// ---------------------------------------------------------------------------

static void ovl_sdl_draw_captured_frame(float dim) {
	if (!s_captureSurface || !s_renderSurface)
		return;

	// Blit captured frame onto render surface
	SDL_SetSurfaceBlendMode(s_captureSurface, SDL_BLENDMODE_NONE);
	SDL_BlitSurface(s_captureSurface, NULL, s_renderSurface, NULL);

	// Apply dimming: overlay a semi-transparent black rect
	// dim=0.4 means final brightness is 40%, so overlay alpha = 60% = 153/255
	if (dim < 1.0f) {
		int alpha = (int)((1.0f - dim) * 255.0f + 0.5f);
		if (alpha > 255)
			alpha = 255;
		if (alpha < 0)
			alpha = 0;

		SDL_Surface* dimSurf = SDL_CreateRGBSurfaceWithFormat(
			0, 1, 1, 32, SDL_PIXELFORMAT_ARGB8888);
		if (dimSurf) {
			SDL_FillRect(dimSurf, NULL,
						 SDL_MapRGBA(dimSurf->format, 0, 0, 0, (Uint8)alpha));
			SDL_SetSurfaceBlendMode(dimSurf, SDL_BLENDMODE_BLEND);
			SDL_Rect dst_rect = {0, 0, s_screenW, s_screenH};
			SDL_BlitScaled(dimSurf, NULL, s_renderSurface, &dst_rect);
			SDL_FreeSurface(dimSurf);
		}
	}
}

// ---------------------------------------------------------------------------
// draw_rect — filled rectangle with alpha support
// ---------------------------------------------------------------------------

static void ovl_sdl_draw_rect(int x, int y, int w, int h, uint32_t color) {
	if (!s_renderSurface)
		return;

	// Extract ARGB components
	uint8_t a = (uint8_t)((color >> 24) & 0xFF);
	uint8_t r = (uint8_t)((color >> 16) & 0xFF);
	uint8_t g = (uint8_t)((color >> 8) & 0xFF);
	uint8_t b = (uint8_t)((color) & 0xFF);

	if (a == 255) {
		// Fully opaque: direct fill
		SDL_Rect rect = {x, y, w, h};
		SDL_FillRect(s_renderSurface, &rect,
					 SDL_MapRGBA(s_renderSurface->format, r, g, b, 255));
	} else {
		// Semi-transparent: use a temp surface and blit with blend
		SDL_Surface* tmp = SDL_CreateRGBSurfaceWithFormat(
			0, 1, 1, 32, SDL_PIXELFORMAT_ARGB8888);
		if (!tmp)
			return;

		SDL_FillRect(tmp, NULL, SDL_MapRGBA(tmp->format, r, g, b, a));
		SDL_SetSurfaceBlendMode(tmp, SDL_BLENDMODE_BLEND);

		SDL_Rect dst_rect = {x, y, w, h};
		SDL_BlitScaled(tmp, NULL, s_renderSurface, &dst_rect);
		SDL_FreeSurface(tmp);
	}
}

// ---------------------------------------------------------------------------
// draw_text — TTF rendered text
// ---------------------------------------------------------------------------

static TTF_Font* get_font(int font_id) {
	if (font_id >= 0 && font_id <= 2 && s_fonts[font_id])
		return s_fonts[font_id];
	// Fallback to small
	return s_fonts[EMU_OVL_FONT_SMALL];
}

static void ovl_sdl_draw_text(const char* text, int x, int y, uint32_t color, int font_id) {
	if (!text || !*text || !s_renderSurface)
		return;

	TTF_Font* font = get_font(font_id);
	if (!font)
		return;

	// Extract ARGB -> SDL_Color (RGBA)
	SDL_Color sdl_color;
	sdl_color.r = (uint8_t)((color >> 16) & 0xFF);
	sdl_color.g = (uint8_t)((color >> 8) & 0xFF);
	sdl_color.b = (uint8_t)((color) & 0xFF);
	sdl_color.a = (uint8_t)((color >> 24) & 0xFF);

	SDL_Surface* text_surf = TTF_RenderUTF8_Blended(font, text, sdl_color);
	if (!text_surf)
		return;

	SDL_SetSurfaceBlendMode(text_surf, SDL_BLENDMODE_BLEND);
	SDL_Rect dst_rect = {x, y, text_surf->w, text_surf->h};
	SDL_BlitSurface(text_surf, NULL, s_renderSurface, &dst_rect);
	SDL_FreeSurface(text_surf);
}

// ---------------------------------------------------------------------------
// text_width / text_height
// ---------------------------------------------------------------------------

static int ovl_sdl_text_width(const char* text, int font_id) {
	if (!text || !*text)
		return 0;

	TTF_Font* font = get_font(font_id);
	if (!font)
		return 0;

	int w = 0;
	TTF_SizeUTF8(font, text, &w, NULL);
	return w;
}

static int ovl_sdl_text_height(int font_id) {
	TTF_Font* font = get_font(font_id);
	if (!font)
		return 0;
	return TTF_FontHeight(font);
}

// ---------------------------------------------------------------------------
// begin_frame — save GL state, clear render surface
// ---------------------------------------------------------------------------

static void ovl_sdl_begin_frame(void) {
	// Save GL state (must restore ALL state overlay touches to avoid
	// corrupting GLideN64's CachedFunctions state -- especially on PowerVR)
	glGetIntegerv(GL_VIEWPORT, s_savedViewport);
	glGetIntegerv(GL_SCISSOR_BOX, s_savedScissorBox);
	s_savedBlend = glIsEnabled(GL_BLEND);
	s_savedDepthTest = glIsEnabled(GL_DEPTH_TEST);
	s_savedCullFace = glIsEnabled(GL_CULL_FACE);
	s_savedScissorTest = glIsEnabled(GL_SCISSOR_TEST);
	glGetIntegerv(GL_BLEND_SRC_RGB, &s_savedBlendSrcRGB);
	glGetIntegerv(GL_BLEND_DST_RGB, &s_savedBlendDstRGB);
	glGetIntegerv(GL_BLEND_SRC_ALPHA, &s_savedBlendSrcAlpha);
	glGetIntegerv(GL_BLEND_DST_ALPHA, &s_savedBlendDstAlpha);
	glGetIntegerv(GL_CURRENT_PROGRAM, &s_savedProgram);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s_savedVAO);
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s_savedVBO);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &s_savedActiveTexUnit);
	glActiveTexture(GL_TEXTURE0);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &s_savedTex0);
	glGetIntegerv(GL_UNPACK_ALIGNMENT, &s_savedUnpackAlignment);

	// Clear the render surface to fully transparent
	if (s_renderSurface) {
		SDL_FillRect(s_renderSurface, NULL, 0x00000000);
	}
}

// ---------------------------------------------------------------------------
// end_frame — upload surface to GL, draw fullscreen quad, restore GL state
// ---------------------------------------------------------------------------

static void convert_argb_to_rgba(const uint8_t* src, uint8_t* dst, int w, int h, int src_pitch) {
	for (int y = 0; y < h; y++) {
		const uint32_t* src_row = (const uint32_t*)(src + (size_t)y * (size_t)src_pitch);
		uint32_t* dst_row = (uint32_t*)(dst + (size_t)y * (size_t)w * 4);

		for (int x = 0; x < w; x++) {
			uint32_t argb = src_row[x];
			// ARGB: A[31:24] R[23:16] G[15:8] B[7:0]
			// RGBA: R[31:24] G[23:16] B[15:8] A[7:0] (as uint32 on big-endian)
			// But GL_RGBA + GL_UNSIGNED_BYTE reads bytes as R,G,B,A regardless of endian
			uint8_t a = (uint8_t)((argb >> 24) & 0xFF);
			uint8_t r = (uint8_t)((argb >> 16) & 0xFF);
			uint8_t g = (uint8_t)((argb >> 8) & 0xFF);
			uint8_t b = (uint8_t)((argb) & 0xFF);
			uint8_t* p = (uint8_t*)&dst_row[x];
			p[0] = r;
			p[1] = g;
			p[2] = b;
			p[3] = a;
		}
	}
}

static void ovl_sdl_end_frame(void) {
	if (!s_renderSurface || !s_uploadBuffer)
		return;

	// Convert ARGB8888 surface to RGBA byte order for GL upload
	SDL_LockSurface(s_renderSurface);
	convert_argb_to_rgba(
		(const uint8_t*)s_renderSurface->pixels,
		s_uploadBuffer,
		s_screenW, s_screenH,
		s_renderSurface->pitch);
	SDL_UnlockSurface(s_renderSurface);

	// Set overlay GL state
	glViewport(0, 0, s_screenW, s_screenH);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	// Upload composited surface as GL texture
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, s_overlayTexture);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, s_screenW, s_screenH, 0,
				 GL_RGBA, GL_UNSIGNED_BYTE, s_uploadBuffer);

	// Draw fullscreen quad (surface is top-down, GL is bottom-up => flip V)
	float verts[] = {
		// pos.x, pos.y, u, v
		-1.0f,
		-1.0f,
		0.0f,
		1.0f,
		1.0f,
		-1.0f,
		1.0f,
		1.0f,
		-1.0f,
		1.0f,
		0.0f,
		0.0f,
		1.0f,
		-1.0f,
		1.0f,
		1.0f,
		1.0f,
		1.0f,
		1.0f,
		0.0f,
		-1.0f,
		1.0f,
		0.0f,
		0.0f,
	};

	glUseProgram(s_texProgram);
	glUniform1i(s_texLocTexture, 0);

	glBindVertexArray(s_texVAO);
	glBindBuffer(GL_ARRAY_BUFFER, s_texVBO);
	glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)sizeof(verts), verts);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glBindVertexArray(0);
	glBindTexture(GL_TEXTURE_2D, 0);
	glUseProgram(0);

	// Restore all saved GL state
	glViewport(s_savedViewport[0], s_savedViewport[1],
			   s_savedViewport[2], s_savedViewport[3]);
	glScissor(s_savedScissorBox[0], s_savedScissorBox[1],
			  s_savedScissorBox[2], s_savedScissorBox[3]);

	if (s_savedBlend)
		glEnable(GL_BLEND);
	else
		glDisable(GL_BLEND);
	if (s_savedDepthTest)
		glEnable(GL_DEPTH_TEST);
	else
		glDisable(GL_DEPTH_TEST);
	if (s_savedCullFace)
		glEnable(GL_CULL_FACE);
	else
		glDisable(GL_CULL_FACE);
	if (s_savedScissorTest)
		glEnable(GL_SCISSOR_TEST);
	else
		glDisable(GL_SCISSOR_TEST);

	glBlendFuncSeparate(s_savedBlendSrcRGB, s_savedBlendDstRGB,
						s_savedBlendSrcAlpha, s_savedBlendDstAlpha);

	// Restore object bindings (critical for GLideN64's cached state on PowerVR)
	glUseProgram(s_savedProgram);
	glBindVertexArray(s_savedVAO);
	glBindBuffer(GL_ARRAY_BUFFER, s_savedVBO);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, s_savedTex0);
	glActiveTexture(s_savedActiveTexUnit);
	glPixelStorei(GL_UNPACK_ALIGNMENT, s_savedUnpackAlignment);
}

// ---------------------------------------------------------------------------
// Icon loading/drawing (PNG images for button hints)
// ---------------------------------------------------------------------------

static int ovl_sdl_load_icon(const char* path, int target_height) {
	if (!path || s_iconCount >= MAX_ICONS || target_height <= 0)
		return -1;

	SDL_Surface* raw = IMG_Load(path);
	if (!raw) {
		fprintf(stderr, "[OverlaySDL] IMG_Load(%s) failed: %s\n", path, IMG_GetError());
		return -1;
	}

	// Scale to target height preserving aspect ratio
	int scaled_w = (int)((float)raw->w * (float)target_height / (float)raw->h + 0.5f);
	SDL_Surface* argb = SDL_CreateRGBSurfaceWithFormat(
		0, scaled_w, target_height, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!argb) {
		SDL_FreeSurface(raw);
		return -1;
	}

	SDL_SetSurfaceBlendMode(raw, SDL_BLENDMODE_NONE);
	SDL_Rect dst = {0, 0, scaled_w, target_height};
	SDL_BlitScaled(raw, NULL, argb, &dst);
	SDL_FreeSurface(raw);

	int id = s_iconCount++;
	s_icons[id] = argb;
	return id;
}

static void ovl_sdl_draw_icon(int icon_id, int x, int y) {
	if (icon_id < 0 || icon_id >= s_iconCount || !s_icons[icon_id] || !s_renderSurface)
		return;

	SDL_SetSurfaceBlendMode(s_icons[icon_id], SDL_BLENDMODE_BLEND);
	SDL_Rect dst = {x, y, s_icons[icon_id]->w, s_icons[icon_id]->h};
	SDL_BlitSurface(s_icons[icon_id], NULL, s_renderSurface, &dst);
}

static int ovl_sdl_icon_width(int icon_id) {
	if (icon_id < 0 || icon_id >= s_iconCount || !s_icons[icon_id])
		return 0;
	return s_icons[icon_id]->w;
}

static int ovl_sdl_icon_height(int icon_id) {
	if (icon_id < 0 || icon_id >= s_iconCount || !s_icons[icon_id])
		return 0;
	return s_icons[icon_id]->h;
}

static void ovl_sdl_free_icon(int icon_id) {
	if (icon_id < 0 || icon_id >= s_iconCount || !s_icons[icon_id])
		return;
	SDL_FreeSurface(s_icons[icon_id]);
	s_icons[icon_id] = NULL;
}

// ---------------------------------------------------------------------------
// save_captured_frame — write captured frame as PNG
// ---------------------------------------------------------------------------

static int ovl_sdl_save_captured_frame(const char* path) {
	if (!path || !s_captureSurface)
		return -1;
	if (SDL_SaveBMP(s_captureSurface, path) != 0) {
		fprintf(stderr, "[OverlaySDL] SDL_SaveBMP(%s) failed: %s\n", path, SDL_GetError());
		return -1;
	}
	return 0;
}

// ---------------------------------------------------------------------------
// Backend struct
// ---------------------------------------------------------------------------

static EmuOvlRenderBackend s_backend = {
	ovl_sdl_init,
	ovl_sdl_destroy,
	ovl_sdl_draw_rect,
	ovl_sdl_draw_text,
	ovl_sdl_text_width,
	ovl_sdl_text_height,
	ovl_sdl_begin_frame,
	ovl_sdl_end_frame,
	ovl_sdl_capture_frame,
	ovl_sdl_draw_captured_frame,
	ovl_sdl_load_icon,
	ovl_sdl_free_icon,
	ovl_sdl_draw_icon,
	ovl_sdl_icon_width,
	ovl_sdl_icon_height,
	ovl_sdl_save_captured_frame};

EmuOvlRenderBackend* overlay_sdl_get_backend(void) {
	return &s_backend;
}
