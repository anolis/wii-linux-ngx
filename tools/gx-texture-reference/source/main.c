/*
 * Known-good libogc RGB565 texture stream for the Linux GX driver.
 *
 * The texture layout and GX pipeline intentionally mirror gcn-gx.c closely
 * enough that a Dolphin FIFO capture can serve as an ordered command reference.
 */

#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include <gccore.h>

#define FIFO_SIZE (256 * 1024)
#define TEX_WIDTH 640
#define TEX_HEIGHT 480

static GXRModeObj *video_mode;
static void *framebuffer;
static u16 *texture_data;
static GXTexObj texture_object;

static u16 pattern_pixel(u16 x, u16 y)
{
	u16 color;

	if (x < TEX_WIDTH / 2 && y < TEX_HEIGHT / 2)
		color = 0xf800;
	else if (x >= TEX_WIDTH / 2 && y < TEX_HEIGHT / 2)
		color = 0x07e0;
	else if (x < TEX_WIDTH / 2)
		color = 0x001f;
	else
		color = 0xffff;

	if ((x % 32) == 0 || (y % 32) == 0)
		color = 0x0000;
	if (x == y || x + y == TEX_WIDTH - 1)
		color = 0xffe0;

	return color;
}

static void fill_tiled_rgb565(void)
{
	u16 tile_x, tile_y, x, y;
	u32 tiles_per_row = TEX_WIDTH / 4;

	for (tile_y = 0; tile_y < TEX_HEIGHT / 4; tile_y++) {
		for (tile_x = 0; tile_x < TEX_WIDTH / 4; tile_x++) {
			u32 tile = (tile_y * tiles_per_row + tile_x) * 16;

			for (y = 0; y < 4; y++) {
				for (x = 0; x < 4; x++) {
					u16 px = tile_x * 4 + x;
					u16 py = tile_y * 4 + y;

					texture_data[tile + y * 4 + x] =
						pattern_pixel(px, py);
				}
			}
		}
	}

	DCFlushRange(texture_data, TEX_WIDTH * TEX_HEIGHT * sizeof(*texture_data));
}

static void draw_textured_quad(f32 width, f32 height)
{
	GX_Begin(GX_QUADS, GX_VTXFMT0, 4);

	GX_Position2f32(0.0f, 0.0f);
	GX_Color4u8(0xff, 0x00, 0x00, 0xff);

	GX_Position2f32(width, 0.0f);
	GX_Color4u8(0xff, 0x00, 0x00, 0xff);

	GX_Position2f32(width, height);
	GX_Color4u8(0xff, 0x00, 0x00, 0xff);

	GX_Position2f32(0.0f, height);
	GX_Color4u8(0xff, 0x00, 0x00, 0xff);

	GX_End();
}

static void configure_gx(void)
{
	GXColor clear = {0x00, 0xff, 0x00, 0xff};
	Mtx modelview;
	Mtx texture_matrix;
	Mtx44 projection;
	f32 yscale;

	GX_SetCopyClear(clear, GX_MAX_Z24);
	GX_SetViewport(0.0f, 0.0f, video_mode->fbWidth,
		       video_mode->efbHeight, 0.0f, 1.0f);
	yscale = GX_GetYScaleFactor(video_mode->efbHeight,
				   video_mode->xfbHeight);
	GX_SetDispCopyYScale(yscale);
	GX_SetScissor(0, 0, video_mode->fbWidth, video_mode->efbHeight);
	GX_SetDispCopySrc(0, 0, video_mode->fbWidth, video_mode->efbHeight);
	GX_SetDispCopyDst(video_mode->fbWidth, video_mode->xfbHeight);
	GX_SetCopyFilter(video_mode->aa, video_mode->sample_pattern,
			 GX_TRUE, video_mode->vfilter);
	GX_SetFieldMode(video_mode->field_rendering,
			(video_mode->viHeight == 2 * video_mode->xfbHeight));
	GX_SetDispCopyGamma(GX_GM_1_0);

	GX_ClearVtxDesc();
	GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
	GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
	GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);

	GX_SetNumChans(1);
	GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX,
		       GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
	GX_SetNumTexGens(1);
	guMtxIdentity(texture_matrix);
	texture_matrix[0][0] = 1.0f / TEX_WIDTH;
	texture_matrix[1][1] = 1.0f / TEX_HEIGHT;
	GX_LoadTexMtxImm(texture_matrix, GX_TEXMTX0, GX_MTX2x4);
	GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4,
			  GX_TG_POS, GX_TEXMTX0);
	GX_LoadTexObj(&texture_object, GX_TEXMAP0);
	GX_InvalidateTexAll();

	GX_SetNumTevStages(1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0,
		       GX_TEXMAP0, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
	GX_SetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);
	GX_SetTevSwapModeTable(GX_TEV_SWAP0, GX_CH_RED, GX_CH_GREEN,
			       GX_CH_BLUE, GX_CH_ALPHA);

	GX_SetCullMode(GX_CULL_NONE);
	GX_SetClipMode(GX_CLIP_ENABLE);
	GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
	GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_COPY);
	GX_SetColorUpdate(GX_TRUE);
	GX_SetAlphaUpdate(GX_TRUE);
	GX_SetDstAlpha(GX_DISABLE, 0);
	GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
	GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);

	guMtxIdentity(modelview);
	GX_LoadPosMtxImm(modelview, GX_PNMTX0);
	GX_SetCurrentMtx(GX_PNMTX0);
	guOrtho(projection, 0.0f, video_mode->efbHeight,
		0.0f, video_mode->fbWidth, 0.0f, 1.0f);
	GX_LoadProjectionMtx(projection, GX_ORTHOGRAPHIC);
}

int main(void)
{
	void *fifo;

	VIDEO_Init();
	video_mode = VIDEO_GetPreferredMode(NULL);
	if (video_mode->fbWidth != TEX_WIDTH ||
	    video_mode->efbHeight != TEX_HEIGHT)
		return EXIT_FAILURE;

	framebuffer = MEM_K0_TO_K1(SYS_AllocateFramebuffer(video_mode));
	texture_data = memalign(32, TEX_WIDTH * TEX_HEIGHT * sizeof(*texture_data));
	if (!framebuffer || !texture_data)
		return EXIT_FAILURE;
	fill_tiled_rgb565();

	VIDEO_Configure(video_mode);
	VIDEO_SetNextFramebuffer(framebuffer);
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	VIDEO_WaitVSync();

	fifo = MEM_K0_TO_K1(memalign(32, FIFO_SIZE));
	if (!fifo)
		return EXIT_FAILURE;
	memset(fifo, 0, FIFO_SIZE);
	GX_Init(fifo, FIFO_SIZE);
	GX_InitTexObj(&texture_object, texture_data, TEX_WIDTH, TEX_HEIGHT,
		      GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GX_InitTexObjLOD(&texture_object, GX_NEAR, GX_NEAR,
			 0.0f, 0.0f, 0.0f, GX_DISABLE, GX_DISABLE, GX_ANISO_1);

	while (1) {
		configure_gx();
		draw_textured_quad(video_mode->fbWidth, video_mode->efbHeight);
		GX_DrawDone();
		GX_CopyDisp(framebuffer, GX_TRUE);
		GX_Flush();
		VIDEO_SetNextFramebuffer(framebuffer);
		VIDEO_Flush();
		VIDEO_WaitVSync();
	}
}
