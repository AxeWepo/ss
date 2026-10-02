/*
 * PC Screen - shows your PC screen on the PS3 and sends the PS3 pad back.
 * Build with the PSL1GHT / ps3dev toolchain (same as any other PSL1GHT app).
 *
 * EDIT PC_IP BELOW to your PC's LAN address.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <ppu-types.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <net/socket.h>
#include <net/net.h>

#include <io/pad.h>
#include <sys/thread.h>
#include <sysutil/sysutil.h>

#include "rsxutil.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_THREAD_LOCALS
#include "stb_image.h"

#define PC_IP        "192.168.1.4"   /* <-- your PC */
#define VIDEO_PORT   5000
#define INPUT_PORT   5001

#define MAX_FRAME    (512 * 1024)
#define MAX_SCALE    2                /* integer upscale cap; 2x of 640x360 = 720p */

/* From rsxutil.c (non-static there, not in the header) */
extern u32 *color_buffer[2];
extern u32 color_pitch;

static volatile int running = 1;
static volatile int vsock = -1;
static sys_ppu_thread_t input_thr;

/* ---------------------------------------------------------- exit ---- */
static void sysutil_cb(u64 status, u64 param, void *usrdata)
{
	if (status == SYSUTIL_EXIT_GAME) {
		running = 0;
		if (vsock >= 0) shutdown(vsock, SHUT_RDWR); /* unblocks recv() */
	}
}

/* --------------------------------------------------------- input ---- */
/*
 * Packet (8 bytes): u16 XInput button mask (big-endian), lx, ly, rx, ry, lt, rt
 * The button mask is already in XInput layout so the PC just copies it.
 */
static void input_thread(void *arg)
{
	int s = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
	struct sockaddr_in to;
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = htons(INPUT_PORT);
	to.sin_addr.s_addr = inet_addr(PC_IP);

	while (running) {
		padInfo info;
		padData d;
		u8 pkt[8] = { 0, 0, 128, 128, 128, 128, 0, 0 };
		u32 i;

		sysUtilCheckCallback();   /* also services the PS-button quit */
		ioPadGetInfo(&info);

		for (i = 0; i < MAX_PADS; i++) {
			if (!info.status[i]) continue;
			ioPadGetData(i, &d);

			u16 b = 0;
			if (d.BTN_UP)       b |= 0x0001;
			if (d.BTN_DOWN)     b |= 0x0002;
			if (d.BTN_LEFT)     b |= 0x0004;
			if (d.BTN_RIGHT)    b |= 0x0008;
			if (d.BTN_START)    b |= 0x0010;
			if (d.BTN_SELECT)   b |= 0x0020;  /* -> Back */
			if (d.BTN_L3)       b |= 0x0040;
			if (d.BTN_R3)       b |= 0x0080;
			if (d.BTN_L1)       b |= 0x0100;
			if (d.BTN_R1)       b |= 0x0200;
			if (d.BTN_CROSS)    b |= 0x1000;  /* A */
			if (d.BTN_CIRCLE)   b |= 0x2000;  /* B */
			if (d.BTN_SQUARE)   b |= 0x4000;  /* X */
			if (d.BTN_TRIANGLE) b |= 0x8000;  /* Y */

			u32 lt = d.PRE_L2, rt = d.PRE_R2;
			if (d.BTN_L2 && lt == 0) lt = 255; /* non-analog triggers */
			if (d.BTN_R2 && rt == 0) rt = 255;

			pkt[0] = b >> 8;
			pkt[1] = b & 0xff;
			pkt[2] = d.ANA_L_H;
			pkt[3] = d.ANA_L_V;
			pkt[4] = d.ANA_R_H;
			pkt[5] = d.ANA_R_V;
			pkt[6] = lt;
			pkt[7] = rt;
			break;
		}

		sendto(s, pkt, sizeof(pkt), 0, (struct sockaddr *)&to, sizeof(to));
		usleep(8000);             /* ~125 Hz */
	}

	close(s);
	sysThreadExit(0);
}

/* --------------------------------------------------------- video ---- */
static int connect_pc(void)
{
	struct sockaddr_in a;
	int s = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s < 0) return -1;

	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(VIDEO_PORT);
	a.sin_addr.s_addr = inet_addr(PC_IP);

	if (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
		close(s);
		return -1;
	}
	return s;
}

static int recv_all(int s, u8 *buf, u32 len)
{
	u32 got = 0;
	while (got < len) {
		int r = recv(s, buf + got, len - got, 0);
		if (r <= 0) return 0;
		got += r;
	}
	return 1;
}

static void clear_screen(void)
{
	int i;
	for (i = 0; i < 2; i++)
		memset(color_buffer[i], 0, display_height * color_pitch);
}

/* src is RGBA bytes from stb (= 0xRRGGBBAA as a big-endian u32) */
static void blit(const u32 *src, int w, int h)
{
	static u32 line[1920];
	u32 *dst = color_buffer[curr_fb];
	int s = display_width / w;
	int sy = display_height / h;
	int x, y, k;

	if (sy < s) s = sy;
	if (s > MAX_SCALE) s = MAX_SCALE;
	if (s < 1) s = 1;

	int cw = w, ch = h;
	if (cw * s > (int)display_width)  cw = display_width / s;
	if (ch * s > (int)display_height) ch = display_height / s;
	int ox = (display_width - cw * s) / 2;
	int oy = (display_height - ch * s) / 2;

	for (y = 0; y < ch; y++) {
		const u32 *row = src + y * w;
		/* build the scaled row in normal RAM (reading VRAM is very slow) */
		for (x = 0; x < cw; x++) {
			u32 p = row[x] >> 8;          /* 0x00RRGGBB */
			for (k = 0; k < s; k++) line[x * s + k] = p;
		}
		for (k = 0; k < s; k++)
			memcpy(dst + (oy + y * s + k) * display_width + ox, line, cw * s * 4);
	}
}

static void stream(int s, u8 *frame)
{
	while (running) {
		u8 hdr[4];
		if (!recv_all(s, hdr, 4)) return;
		u32 len = ((u32)hdr[0] << 24) | (hdr[1] << 16) | (hdr[2] << 8) | hdr[3];
		if (len == 0 || len > MAX_FRAME) return;
		if (!recv_all(s, frame, len)) return;

		int w, h, n;
		u8 *px = stbi_load_from_memory(frame, len, &w, &h, &n, 4);
		if (!px) continue;

		blit((const u32 *)px, w, h);
		stbi_image_free(px);
		flip();
	}
}

/* ---------------------------------------------------------- main ---- */
int main(int argc, char *argv[])
{
	void *host_addr = memalign(1024 * 1024, HOST_SIZE);
	u8 *frame = (u8 *)malloc(MAX_FRAME);
	u64 ret;

	init_screen(host_addr, HOST_SIZE);
	ioPadInit(7);
	sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sysutil_cb, NULL);
	netInitialize();
	clear_screen();

	sysThreadCreate(&input_thr, input_thread, NULL, 999, 64 * 1024,
	                THREAD_JOINABLE, "input");

	while (running) {
		vsock = connect_pc();
		if (vsock < 0) {
			usleep(500000);     /* PC not up yet, retry */
			continue;
		}
		printf("connected to %s\n", PC_IP);
		stream(vsock, frame);
		close(vsock);
		vsock = -1;
		clear_screen();
	}

	sysThreadJoin(input_thr, &ret);
	netDeinitialize();
	gcmSetWaitFlip(context);
	rsxFinish(context, 1);
	return 0;
}
