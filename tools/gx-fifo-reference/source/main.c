/*
 * Minimal libogc reference stream for the Linux GX driver investigation.
 *
 * This deliberately mirrors the driver's direct-colour diagnostic: direct
 * XY/F32 positions, direct RGBA8 colours, one colour channel, no texture,
 * GX_PASSCLR, an orthographic projection, and a full-screen red quad.
 */

#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include <gccore.h>

#define FIFO_SIZE (256 * 1024)

static GXRModeObj *video_mode;
static void *framebuffer;

static void draw_red_quad(f32 width, f32 height)
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
	GX_SetNumTexGens(0);
	GX_SetNumTevStages(1);
	GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL,
		       GX_TEXMAP_NULL, GX_COLOR0A0);
	GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
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
	framebuffer = MEM_K0_TO_K1(SYS_AllocateFramebuffer(video_mode));

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
	configure_gx();

	while (1) {
		/* Keep the complete ordered draw state in every FIFO capture. */
		configure_gx();
		draw_red_quad(video_mode->fbWidth, video_mode->efbHeight);
		GX_DrawDone();
		GX_CopyDisp(framebuffer, GX_TRUE);
		GX_Flush();
		VIDEO_SetNextFramebuffer(framebuffer);
		VIDEO_Flush();
		VIDEO_WaitVSync();
	}
}
