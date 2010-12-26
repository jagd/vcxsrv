/*
 * libtxc_dxtn
 * Version:  0.1
 *
 * Copyright (C) 2004  Roland Scheidegger   All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * BRIAN PAUL BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
 * AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "base/basictypes.h"

typedef uint32 GLenum;
typedef int32  GLint;
typedef uint8  GLubyte;
typedef uint16 GLushort;
typedef uint32 GLuint;
typedef void   GLvoid;

typedef GLubyte GLchan;
#define UBYTE_TO_CHAN(b)  (b)
#define CHAN_MAX 255
#define RCOMP 0
#define GCOMP 1
#define BCOMP 2
#define ACOMP 3

void fetch_2d_texel_rgb_dxt1(GLint srcRowStride, const GLubyte *pixdata,
			     GLint i, GLint j, GLvoid *texel);
void fetch_2d_texel_rgba_dxt1(GLint srcRowStride, const GLubyte *pixdata,
			     GLint i, GLint j, GLvoid *texel);
void fetch_2d_texel_rgba_dxt3(GLint srcRowStride, const GLubyte *pixdata,
			     GLint i, GLint j, GLvoid *texel);
void fetch_2d_texel_rgba_dxt5(GLint srcRowStride, const GLubyte *pixdata,
			     GLint i, GLint j, GLvoid *texel);

void tx_compress_dxtn(GLint srccomps, GLint width, GLint height,
		      const GLubyte *srcPixData, GLenum destformat,
		      GLubyte *dest, GLint dstRowStride);