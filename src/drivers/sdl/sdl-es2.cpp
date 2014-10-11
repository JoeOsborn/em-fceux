#include "sdl.h"
#include "sdl-es2.h"
#include "../common/vidblit.h"
#include "../../utils/memory.h"
#include "../common/nes_ntsc.h"

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <GLES2/gl2platform.h>

#include <cstring>
#include <cstdlib>
#include <cmath>

#ifndef APIENTRY
#define APIENTRY
#endif

#define NTSC_EMULATION 1
#define NTSC_LEVELS 1

static GLuint s_baseTex = 0;
static GLuint s_kernelTex = 0;
#if NTSC_EMULATION 
static uint8* s_tempXBuf = 0;
#endif
static GLuint quadbuf = 0;
static GLuint prog = 0;

//#include "testpixels.inc"

#if NTSC_EMULATION

#define NUM_CYCLES 3
#define NUM_COLORS 64
#define NUM_CYCLES_TEXTURE 4

// Source code modified from:
// http://wiki.nesdev.com/w/index.php/NTSC_video

// Generate the square wave
static bool InColorPhase(int color, int phase)
{
	return (color + phase) % 12 < 6;
};

// pixel = Pixel color (9-bit) given as input. Bitmask format: "eeellcccc".
// phase = Signal phase. It is a variable that increases by 8 each pixel.
static double NTSCsignal(int pixel, int phase)
{
//    static const double attenuation = 0.7460;
    // Voltage levels, relative to synch voltage
    const double levels[8] = {
        0.350, 0.518, 0.962, 1.550, // Signal low
        1.094, 1.506, 1.962, 1.962  // Signal high
    };

    // Decode the NES color.
    int color = (pixel & 0x0F);    // 0..15 "cccc"
    int level = (pixel >> 4) & 3;  // 0..3  "ll"
//    int emphasis = (pixel >> 6);   // 0..7  "eee"
    if(color > 13) { level = 1;  } // For colors 14..15, level 1 is forced.

    // The square wave for this color alternates between these two voltages:
    float low  = levels[0 + level];
    float high = levels[4 + level];
    if(color == 0) { low = high; } // For color 0, only high level is emitted
    if(color > 12) { high = low; } // For colors 13..15, only low level is emitted

    double signal = InColorPhase(color, phase) ? high : low;

    // When de-emphasis bits are set, some parts of the signal are attenuated:
//    if( ((emphasis & 1) && InColorPhase(0, phase))
//    ||  ((emphasis & 2) && InColorPhase(4, phase))
//    ||  ((emphasis & 4) && InColorPhase(8, phase)) ) signal = signal * attenuation;

    return signal;
}

#if NTSC_LEVELS
static const double sc_low = 0.350, sc_high = 1.962;
static GLuint s_ntscTex[2];
#else
static float s_mins[3];
static float s_maxs[3];
#endif

static void GenKernelTex()
{
#if NTSC_LEVELS
	unsigned char *results[2] = {
		(unsigned char*) calloc(4 * NUM_CYCLES_TEXTURE * NUM_COLORS, sizeof(unsigned char)),
		(unsigned char*) calloc(4 * NUM_CYCLES_TEXTURE * NUM_COLORS, sizeof(unsigned char))
	};

	for (int cycle = 0; cycle < NUM_CYCLES; cycle++) {
		const int phase = cycle * 8;

		for (int color = 0; color < NUM_COLORS; color++) {

			for (int p = 0; p < 4; p++) {
				double signal;

				signal = NTSCsignal(color, phase + p);
				signal = (signal-sc_low) / (sc_high-sc_low);
				results[0][4 * (cycle*NUM_COLORS + color) + p] = 255.0 * signal;

				signal = NTSCsignal(color, phase + p + 4);
				signal = (signal-sc_low) / (sc_high-sc_low);
				results[1][4 * (cycle*NUM_COLORS + color) + p] = 255.0 * signal;
			}
		}
	}

	glActiveTexture(GL_TEXTURE1);
	glGenTextures(2, s_ntscTex);
	glBindTexture(GL_TEXTURE_2D, s_ntscTex[0]);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, NUM_COLORS, NUM_CYCLES_TEXTURE, 0, GL_RGBA, GL_UNSIGNED_BYTE, results[0]);

	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, s_ntscTex[1]);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, NUM_COLORS, NUM_CYCLES_TEXTURE, 0, GL_RGBA, GL_UNSIGNED_BYTE, results[1]);

	glActiveTexture(GL_TEXTURE0);

	free(results[0]);
	free(results[1]);
#else
	double *yiqs = (double*) calloc(3 * NUM_CYCLES * NUM_COLORS, sizeof(double));

 	s_mins[0] = s_mins[1] = s_mins[2] = 0.0f;
 	s_maxs[0] = s_maxs[1] = s_maxs[2] = 0.0f;

	for (int cycle = 0; cycle < NUM_CYCLES; cycle++) {
		for (int color = 0; color < NUM_COLORS; color++) {
			const int phase = cycle * 8;
			const double shift = phase + 3.9;

			float yiq[3] = {0.0f, 0.0f, 0.0f};
			for (int p = 0; p < 8; p++)
			{
				double signal = NTSCsignal(color, phase + p);

				static const double black = 0.518, white = 1.962;
				signal = (signal-black) / (white-black);

				const double level = signal / 8.0;
				yiq[0] += level;
				yiq[1] += level * cos(M_PI * (shift+p) / 6.0);
				yiq[2] += level * sin(M_PI * (shift+p) / 6.0);
			}

			for (int i = 0; i < 3; i++) {
				s_mins[i] = fmin(s_mins[i], yiq[i]);
				s_maxs[i] = fmax(s_maxs[i], yiq[i]);
			}

			const int k = 3 * (cycle * NUM_COLORS + color);
			yiqs[k+0] = yiq[0];
			yiqs[k+1] = yiq[1];
			yiqs[k+2] = yiq[2];
		}
	}

	unsigned char *result = (unsigned char*) calloc(3 * NUM_CYCLES_TEXTURE * NUM_COLORS, sizeof(unsigned char));
	for (int cycle = 0; cycle < NUM_CYCLES; cycle++) {
		for (int color = 0; color < NUM_COLORS; color++) {
			const int k = 3 * (cycle*NUM_COLORS + color);
			for (int i = 0; i < 3; i++) {
				const double clamped = (yiqs[k+i]-s_mins[i]) / (s_maxs[i]-s_mins[i]);
				result[k+i] = (unsigned char) (255.0f * clamped);
			}
		}
	}

	glActiveTexture(GL_TEXTURE1);
	glGenTextures(1, &s_kernelTex);
	glBindTexture(GL_TEXTURE_2D, s_kernelTex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, NUM_COLORS, NUM_CYCLES_TEXTURE, 0, GL_RGB, GL_UNSIGNED_BYTE, result);
	glActiveTexture(GL_TEXTURE0);

	free(yiqs);
	free(result);
#endif
}
#endif

void SetOpenGLPalette(uint8 *data)
{
	SetPaletteBlitToHigh((uint8*)data);
}

void BlitOpenGL(uint8 *buf)
{
#if NTSC_EMULATION
//    buf = s_testPixels;
    for (size_t i = 0; i < 256 * 256; i++) {
//       s_tempXBuf[i] = (uint8) (((buf[i] & 63) * 255.0f) / 63.0f);
       s_tempXBuf[i] = buf[i] & 63;
    }
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 256, GL_LUMINANCE, GL_UNSIGNED_BYTE, s_tempXBuf);
//    buf = s_testPixels;
#else
	Blit8ToHigh(buf, (uint8*)HiBuffer, 256, 240, 256*4, 1, 1);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 256, GL_RGBA, GL_UNSIGNED_BYTE, HiBuffer);
#endif

    // Update uniforms.
    {
        int x, y;
        SDL_GetMouseState(&x, &y);
        GLfloat mousePos[2] = { x / 602.0f, y / 448.0f };
        GLint uMousePosLoc = glGetUniformLocation(prog, "u_mousePos");
        glUniform2fv(uMousePosLoc, 1, mousePos);

#if NTSC_LEVELS
#else
        GLint uLevelMinsLoc = glGetUniformLocation(prog, "u_levelMins");
		glUniform3fv(uLevelMinsLoc, 1, s_mins);
        GLint uLevelMaxsLoc = glGetUniformLocation(prog, "u_levelMaxs");
		glUniform3fv(uLevelMaxsLoc, 1, s_maxs);
#endif
    }

	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	SDL_GL_SwapBuffers();
}

void KillOpenGL(void)
{
	if (s_baseTex) {
		glDeleteTextures(1, &s_baseTex);
	    s_baseTex = 0;
	}
	if (s_kernelTex) {
		glDeleteTextures(1, &s_kernelTex);
	    s_kernelTex = 0;
	}
    if (prog) {
        glDeleteProgram(prog);
        prog = 0;
    }
#if !NTSC_EMULATION
    if (HiBuffer) {
	    free(HiBuffer);
	    HiBuffer = 0;
    }
#endif
}

static GLuint CompileShader(GLenum type, const char *src)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &src, 0);
	glCompileShader(shader);
    return shader;
}

int InitOpenGL(int left,
		int right,
		int top,
		int bottom,
		double xscale,
		double yscale,
		int efx,
		int ipolate,
		int stretchx,
		int stretchy,
		SDL_Surface *screen)
{
#if NTSC_EMULATION
    s_tempXBuf = (uint8*) FCEU_malloc(256 * 256);
#else
	HiBuffer = FCEU_malloc(4*256*256);
	memset(HiBuffer,0x00,4*256*256);
  #ifndef LSB_FIRST
	InitBlitToHigh(4,0xFF000000,0xFF0000,0xFF00,efx&2,0,0);
  #else
	InitBlitToHigh(4,0xFF,0xFF00,0xFF0000,efx&2,0,0);
  #endif
#endif
 
	if(screen->flags & SDL_FULLSCREEN)
	{
		xscale=(double)screen->w / (double)(right-left);
		yscale=(double)screen->h / (double)(bottom-top);
		if(xscale<yscale) yscale = xscale;
		if(yscale<xscale) xscale = yscale;
	}

	{
		int rw=(int)((right-left)*xscale);
		int rh=(int)((bottom-top)*yscale);
		int sx=(screen->w-rw)/2;    // Start x
		int sy=(screen->h-rh)/2;    // Start y

		if(stretchx) { sx=0; rw=screen->w; }
		if(stretchy) { sy=0; rh=screen->h; }
		glViewport(sx, sy, rw, rh);
	}

    glActiveTexture(GL_TEXTURE0);
	glGenTextures(1, &s_baseTex);
	glBindTexture(GL_TEXTURE_2D, s_baseTex);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,ipolate?GL_LINEAR:GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,ipolate?GL_LINEAR:GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 256, 256, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, 0);

#if NTSC_EMULATION
    GenKernelTex();
#endif

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);

    const GLfloat quadverts[4 * 4] = {
        // Packed vertices: x, y, u, v
    	-1.0f, -1.0f, left / 256.0f, bottom / 256.0f,
    	 1.0f, -1.0f, right / 256.0f, bottom / 256.0f,
        -1.0f,  1.0f, left / 256.0f, top / 256.0f, 
    	 1.0f,  1.0f, right / 256.0f, top / 256.0f
    };

	// Create quad vertex buffer.
	glGenBuffers(1, &quadbuf);
	glBindBuffer(GL_ARRAY_BUFFER, quadbuf);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quadverts), quadverts, GL_STATIC_DRAW);

	// Create vertex attribute array.
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, 0);

	// Create shaders and program.
    const char* vert_src =
        "precision mediump float;\n"
        "attribute vec4 vert;\n"
        "varying vec2 v_uv;\n"
        "void main() {\n"
        "v_uv = vec2(602.0, 240.0) * vert.zw;\n"
        "gl_Position = vec4(vert.xy, 0.0, 1.0);\n"
        "}\n";
	GLuint vert_shader = CompileShader(GL_VERTEX_SHADER, vert_src);
    
    const char* frag_src =
#if NTSC_EMULATION
#if NTSC_LEVELS
		"precision highp float;\n"
		"uniform sampler2D u_baseTex;\n"
		"uniform sampler2D u_ntscTex0;\n"
    	"uniform sampler2D u_ntscTex1;\n"
		"uniform vec2 u_mousePos;\n"
  		"#define PIC (3.1415926535 / 6.0)\n"
		"#define IN_SIZE 256.0\n"
		"#define WINDOW_SCALER 2.0\n"
		"const mat3 c_convMat = mat3(\n"
		"    1.0,        1.0,        1.0,       // Y\n"
		"    0.946882,   -0.274788,  -1.108545, // I\n"
		"    0.623557,   -0.635691,  1.709007   // Q\n"
		");                                                                                                  \n"
		"\n"
		"const float c_low = 0.350;\n"
		"const float c_black = 0.518;\n"
		"const float c_white = 1.962;\n"
		"\n"
		"const vec3 u_gammaExp = vec3(2.2/2.0);\n"
    	"const vec4 ones = vec4(1.0);\n"
		"\n"
		"vec3 sampel(in vec4 chromaMask0, in vec4 chromaMask1, in vec4 lumaMask0, in vec4 lumaMask1, in vec2 p)\n"
		"{\n"
		"    vec4 a = PIC * (vec4(0.0, 1.0, 2.0, 3.0) + 3.9 + p.x*8.0 + mod(p.y*8.0, 12.0));\n"
		"    vec2 uv = vec2(\n"
		"        texture2D(u_baseTex, p / (IN_SIZE-1.0)).r * 255.0 / 63.0,\n" // color
		"        mod(p.x + p.y, 3.0) / 3.0\n" // phase
		"    );\n"
		"	 vec4 v0 = (texture2D(u_ntscTex0, uv) * (c_white-c_low) + c_low - c_black) / (c_white-c_black);\n"
		"	 vec4 v1 = (texture2D(u_ntscTex1, uv) * (c_white-c_low) + c_low - c_black) / (c_white-c_black);\n"
    	"    vec3 yiq;\n"
		"    yiq.r = dot(lumaMask0, v0) + dot(lumaMask1, v1);\n"
		"    yiq.g = dot(chromaMask0, v0*cos(a)) + dot(chromaMask1, v1*cos(PIC*4.0+a));\n"
		"    yiq.b = dot(chromaMask0, v0*sin(a)) + dot(chromaMask1, v1*sin(PIC*4.0+a));\n"
    	"    return yiq;\n"
		"}\n"
		"\n"
		"void main(void)                                                                                     \n"
		"{                                                                                                   \n"
    	"    vec2 coord = gl_FragCoord.xy - 0.5;\n"
		"    vec2 p = floor(coord / WINDOW_SCALER);\n"
		"    p.y = 224.0 - p.y;\n"
		"    vec3 yiq;\n"
    	"    yiq =  sampel(vec4(1.0,1.0,1.0,1.0), vec4(1.0,1.0,1.0,1.0), vec4(1.0,1.0,1.0,1.0), vec4(1.0,1.0,1.0,1.0), p);\n"
#if 1
    	// 24 samples chroma, 12 samples luma
		"    yiq += sampel(vec4(1.0,1.0,1.0,1.0), vec4(1.0,1.0,1.0,1.0), vec4(0.0,0.0,0.0,0.0), vec4(0.0,0.0,1.0,1.0), p + vec2(-1.0, 0.0));\n"
		"    yiq += sampel(vec4(1.0,1.0,1.0,1.0), vec4(1.0,1.0,1.0,1.0), vec4(1.0,1.0,0.0,0.0), vec4(0.0,0.0,0.0,0.0), p + vec2(1.0, 0.0));\n"
    	// 24+12 samples chroma, 12 samples luma
//		"    yiq += sampel(vec4(0.0,0.0,1.0,1.0), vec4(1.0,1.0,1.0,1.0), vec4(0.0,0.0,0.0,0.0), vec4(0.0,0.0,0.0,0.0), p + vec2(-2.0, 0.0));\n"
//		"    yiq += sampel(vec4(1.0,1.0,1.0,1.0), vec4(1.0,1.0,0.0,0.0), vec4(0.0,0.0,0.0,0.0), vec4(0.0,0.0,0.0,0.0), p + vec2(2.0, 0.0));\n"
#else
    	// 12 samples chroma & luma
    	"    yiq += sampel(vec4(0.0,0.0,0.0,0.0), vec4(0.0,0.0,1.0,1.0), vec4(0.0,0.0,0.0,0.0), vec4(0.0,0.0,1.0,1.0), p + vec2(-1.0, 0.0));\n"
    	"    yiq += sampel(vec4(1.0,1.0,0.0,0.0), vec4(0.0,0.0,0.0,0.0), vec4(1.0,1.0,0.0,0.0), vec4(0.0,0.0,0.0,0.0), p + vec2(1.0, 0.0));\n"
#endif
   		"    yiq.r *= 0.25 * u_mousePos.y;\n"
		"    yiq.gb *= 0.1 * u_mousePos.x;\n"
    	"    float scan = 1.0 - (7.0/256.0) * mod(coord.y, WINDOW_SCALER) / (WINDOW_SCALER-1.0);\n"
		"    gl_FragColor = vec4(c_convMat * yiq * scan, 1.0);\n"
		"}\n"
		;
#elif 1
    		"precision highp float;\n"
			"uniform sampler2D u_baseTex;\n"
			"uniform sampler2D u_kernelTex;\n"
			"uniform vec2 u_mousePos;\n"
    		"uniform vec3 u_levelMins;\n"
    		"uniform vec3 u_levelMaxs;\n"
			"\n"
			"#define IN_SIZE 256.0\n"
			"#define WINDOW_SCALER 2.0\n"
			"const mat3 c_convMat = mat3(\n"
			"    1.0,        1.0,        1.0,        // Y\n"
			"    0.946882,   -0.274788,  -1.108545,  // I\n"
			"    0.623557,   -0.635691,  1.709007    // Q\n"
			");\n"
			"\n"
			"const float c_black = 0.518;\n"
			"const float c_white = 1.962;\n"
			"\n"
			"const vec3 u_gammaExp = vec3(2.2/2.0);\n"
			"\n"
    		"vec3 sampel(in vec2 p)\n"
			"{\n"
    		"	vec2 uv = vec2(\n"
    		"		texture2D(u_baseTex, p / (IN_SIZE-1.0)).r * 255.0 / 63.0,\n" // color
    		"		mod(p.x + p.y, 3.0) / 3.0\n" // phase
    		"	);\n"
    		"	return texture2D(u_kernelTex, uv).rgb * (u_levelMaxs-u_levelMins) + u_levelMins;\n"
    		"}\n"
    		"\n"
			"void main(void)\n"
			"{\n"
			"    vec2 p = floor((gl_FragCoord.xy - 0.5) / WINDOW_SCALER);\n"
			"    p.y = 224.0 - p.y;\n"
    		"    vec3 yiq = vec3(0.0);\n"
    		"    yiq += sampel(p);\n"
    		"    yiq += sampel(p + vec2(-1.0, 0.0));\n"
    		"    yiq += sampel(p + vec2(1.0, 0.0));\n"
			"    yiq.gb *= u_mousePos.x;\n"
    		"    yiq.r *= u_mousePos.y;\n"
			"    gl_FragColor = vec4(c_convMat * yiq, 1.0);\n"
			"}\n"
			;
#else
    		"precision highp float;                                                                              \n"
            "uniform sampler2D u_baseTex;\n"
            "uniform sampler2D u_kernelTex;\n"
            "uniform vec2 u_mousePos;\n"
    		"                                                                                                    \n"
    		"#define HALF_SAMPLES 6.0                                                                   \n"
    		"#define SAMPLES (2.0*HALF_SAMPLES)                                                                  \n"
    		"#define IN_SIZE 256.0                                                                  \n"
    		"#define WINDOW_SCALER 2.0                                                                  \n"
    		"#define PI 3.1415926535                                                                             \n"

    		"const mat3 c_convMat = mat3(                                                                        \n"
    		"    1.0,        1.0,        1.0,        // Y multiplier                                             \n"
    		"    0.946882,   -0.274788,  -1.108545,  // I '                                                      \n"
    		"    0.623557,   -0.635691,  1.709007    // Q '                                                      \n"
    		");                                                                                                  \n"

    		"                                                                                                    \n"
    		"const float c_black = 0.518;                                                                        \n"
    		"const float c_white = 1.962;                                                                        \n"
    		"                                                                                                    \n"
    		"const vec3 u_gammaExp = vec3(2.2/2.0);                                                              \n"
    		"                                                                                                    \n"
    		"// pixel = Pixel color (9-bit) given as input. Bitmask format: 'eeellcccc'.                         \n"
    		"// phase = Signal phase (0..11). It is a variable that increases by 8 each pixel.                   \n"
    		"float NTSCsignal(in float pixel, in float phase)                                                    \n"
    		"{                                                                                                   \n"
    		"    vec4 levelsLow = vec4(0.350, 0.518, 0.962, 1.550);                                              \n"
    		"    vec4 levelsHigh = vec4(1.094, 1.506, 1.962, 1.962);                                             \n"
    		"                                                                                                    \n"
    		"    // Decode the NES color.                                                                        \n"
    		"    float color = mod(floor(pixel * 256.0), 16.0);                                           \n"
    		"    float level = mod(floor(pixel * 16.0), 4.0);                                             \n"
    		//"    int emphasis = (pixel >> 6);                                                                  \n"
    		"    if (color >= 14.0) { level = 1.0; }                                                             \n"
    		"    if (color >= 13.0) { levelsHigh = levelsLow; }                                                  \n"
    		"    if (color == 0.0) { levelsLow = levelsHigh; }                                                   \n"
    		//"    vec4 result = (mod(color + phase, 12.0) < 6.0) ? levelsHigh : levelsLow;\n"
    		"    vec4 result = mix(levelsLow, levelsHigh, step(mod(color + phase, 12.0), 6.0));\n"
    		"    if (level >= 3.0) { return result[3]; }                                                      \n"
    		"    else if (level >= 2.0) { return result[2]; }                                                 \n"
    		"    else if (level >= 1.0) { return result[1]; }                                                 \n"
    		"    else { return result[0]; }                                                                   \n"
    		"                                                                                                    \n"
    		"    // When de-emphasis bits are set, some parts of the signal are attenuated:                      \n"
    		//"   if( ((emphasis & 1) && InColorPhase(0))                                                        \n"
    		//"    ||  ((emphasis & 2) && InColorPhase(4))                                                       \n"
    		//"    ||  ((emphasis & 4) && InColorPhase(8)) ) signal = signal * attenuation;                      \n"
    		"}                                                                                                   \n"
    		"                                                                                                    \n"
    		"float signalLevel(in float phase, in float cycle, in float y)                                       \n"
    		"{                                                                                                   \n"
    		"    vec2 uv = vec2(cycle/8.0, y) / (IN_SIZE-1.0);                                                   \n"
    		"    vec4 pixel = texture2D(u_baseTex, uv);                                                          \n"
    		"    float signal = NTSCsignal(pixel.r, phase);                                                      \n"
    		"    return (signal-c_black) / (c_white-c_black);                                                    \n"
    		//"    return signal;\n"
    		"}                                                                                                   \n"
    		"                                                                                                    \n"
    		"void main(void)                                                                                     \n"
    		"{                                                                                                   \n"
    		"    vec2 p = floor((gl_FragCoord.xy - 0.5) / WINDOW_SCALER);                                \n"
    		"    p.y = 224.0 - p.y;                                                                          \n"
    		"    float begin = p.x * 8.0;\n"
    		"    vec3 yiq = vec3(0.0);                                                                           \n"
//    		"    float phase = mod(8.0 * (p.x + mod(p.y, 3.0)), 12.0);                                           \n"
    		"    float phase = mod(begin, 12.0);                                           \n"
    		"    for (float s = -HALF_SAMPLES; s < HALF_SAMPLES; s += 1.0)                                       \n"
    		"    {                                                                                               \n"
    		//"        float cycle = begin + s;                                                                    \n"
    		"        float level = signalLevel(phase, begin + s, p.y) / SAMPLES;                                     \n"
    		"        float a = PI * (phase+begin + s) / 6.0;                                                     \n"
    		"        yiq += level * vec3(1.0, cos(a), sin(a));                                                   \n"
    		"    }                                                                                               \n"
    		"    yiq.gb *= 4.0 * u_mousePos.x;\n"
    		"    gl_FragColor = vec4(                                                                            \n"
    		//"        clamp(pow(max(c_convMat * yiq, 0.0), u_gammaExp), 0.0, 1.0),                                \n"
    		"        clamp(max(c_convMat * yiq, 0.0), 0.0, 1.0),                                \n"
    		"        1.0                                                                                         \n"
    		"    );                                                                                              \n"
    		"}                                                                                                   \n"
    		;
#endif
#else
        "precision lowp float;\n"
        "uniform sampler2D u_baseTex;\n"
        "varying vec2 v_uv;\n"
        "void main() {\n"
        "gl_FragColor = texture2D(u_baseTex, v_uv);\n"
        "}\n";
#endif
	GLuint frag_shader = CompileShader(GL_FRAGMENT_SHADER, frag_src);

	prog = glCreateProgram();
	glAttachShader(prog, vert_shader);
	glAttachShader(prog, frag_shader);
	glLinkProgram(prog);
	glDetachShader(prog, vert_shader);
	glDetachShader(prog, frag_shader);
	glDeleteShader(vert_shader);
	glDeleteShader(frag_shader);
	glUseProgram(prog);

    GLint uBaseTexLoc = glGetUniformLocation(prog, "u_baseTex");
    glUniform1i(uBaseTexLoc, 0);
#if NTSC_LEVELS
    GLint uNtscTexLoc = glGetUniformLocation(prog, "u_ntscTex0");
    glUniform1i(uNtscTexLoc, 1);
    uNtscTexLoc = glGetUniformLocation(prog, "u_ntscTex1");
    glUniform1i(uNtscTexLoc, 2);
#else
    GLint uKernelTexLoc = glGetUniformLocation(prog, "u_kernelTex");
    glUniform1i(uKernelTexLoc, 1);
#endif
    GLfloat mousePos[2] = { 0.0f, 0.0f };
    GLint uMousePosLoc = glGetUniformLocation(prog, "u_mousePos");
    glUniform2fv(uMousePosLoc, 1, mousePos);

	// In a double buffered setup with page flipping, be sure to clear both buffers.
	glClear(GL_COLOR_BUFFER_BIT);
	SDL_GL_SwapBuffers();
	glClear(GL_COLOR_BUFFER_BIT);
	SDL_GL_SwapBuffers();

	return 1;
}

