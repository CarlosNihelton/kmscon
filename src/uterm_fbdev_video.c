/*
 * uterm - Linux User-Space Terminal fbdev module
 *
 * Copyright (c) 2011-2013 David Herrmann <dh.herrmann@googlemail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * FBDEV Video backend
 */

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "log.h"
#include "uterm_fbdev_internal.h"
#include "uterm_video.h"
#include "uterm_video_internal.h"

#define LOG_SUBSYSTEM "video_fbdev"

static int mode_init(struct uterm_mode *mode)
{
	struct fbdev_mode *fbdev;

	fbdev = malloc(sizeof(*fbdev));
	if (!fbdev)
		return -ENOMEM;
	memset(fbdev, 0, sizeof(*fbdev));
	mode->data = fbdev;

	return 0;
}

static void mode_destroy(struct uterm_mode *mode)
{
	free(mode->data);
}

static const char *mode_get_name(const struct uterm_mode *mode)
{
	return "<default>";
}

static unsigned int mode_get_width(const struct uterm_mode *mode)
{
	struct fbdev_mode *fbdev = mode->data;

	return fbdev->width;
}

static unsigned int mode_get_height(const struct uterm_mode *mode)
{
	struct fbdev_mode *fbdev = mode->data;

	return fbdev->height;
}

static const struct mode_ops fbdev_mode_ops = {
	.init = mode_init,
	.destroy = mode_destroy,
	.get_name = mode_get_name,
	.get_width = mode_get_width,
	.get_height = mode_get_height,
};

static int refresh_info(struct uterm_display *disp)
{
	int ret;
	struct fbdev_display *fbdev = disp->data;

	ret = ioctl(fbdev->fd, FBIOGET_FSCREENINFO, &fbdev->finfo);
	if (ret) {
		log_err("cannot get finfo (%d): %m", errno);
		return -EFAULT;
	}

	ret = ioctl(fbdev->fd, FBIOGET_VSCREENINFO, &fbdev->vinfo);
	if (ret) {
		log_err("cannot get vinfo (%d): %m", errno);
		return -EFAULT;
	}

	return 0;
}

static int display_activate_force(struct uterm_display *disp,
				  struct uterm_mode *mode,
				  bool force)
{
	/* TODO: Add support for 24-bpp. However, we need to check how 3-bytes
	 * integers are assembled in big/little/mixed endian systems. */
	static const char depths[] = { 32, 16, 0 };
	struct fb_var_screeninfo *vinfo;
	struct fb_fix_screeninfo *finfo;
	int ret, i;
	uint64_t quot;
	size_t len;
	unsigned int val;
	struct fbdev_display *fbdev = disp->data;
	struct fbdev_mode *fbdev_mode;
	struct uterm_mode *m;

	if (!disp->video || !video_is_awake(disp->video))
		return -EINVAL;
	if (!force && (disp->flags & DISPLAY_ONLINE))
		return 0;

	/* TODO: We do not support explicit modesetting in fbdev, so we require
	 * @mode to be NULL. You can still switch modes via "fbset" on the
	 * console and then restart the app. It will automatically adapt to the
	 * new mode. The only values changed here are bpp and color mode. */
	if (mode)
		return -EINVAL;

	ret = refresh_info(disp);
	if (ret)
		return ret;

	finfo = &fbdev->finfo;
	vinfo = &fbdev->vinfo;

	vinfo->xoffset = 0;
	vinfo->yoffset = 0;
	vinfo->activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
	vinfo->xres_virtual = vinfo->xres;
	vinfo->yres_virtual = vinfo->yres * 2;
	disp->flags |= DISPLAY_DBUF;

	/* udlfb is broken as it reports the sizes of the virtual framebuffer
	 * (even mmap() accepts it) but the actual size that we can access
	 * without segfaults is the _real_ framebuffer. Therefore, disable
	 * double-buffering for it.
	 * TODO: fix this kernel-side!
	 * TODO: There are so many broken fbdev drivers that just accept any
	 * virtual FB sizes and then break mmap that we now disable
	 * double-buffering entirely. We might instead add a white-list or
	 * optional command-line argument to re-enable it. */
	if (true || !strcmp(finfo->id, "udlfb")) {
		disp->flags &= ~DISPLAY_DBUF;
		vinfo->yres_virtual = vinfo->yres;
	}

	ret = ioctl(fbdev->fd, FBIOPUT_VSCREENINFO, vinfo);
	if (ret) {
		disp->flags &= ~DISPLAY_DBUF;
		vinfo->yres_virtual = vinfo->yres;
		ret = ioctl(fbdev->fd, FBIOPUT_VSCREENINFO, vinfo);
		if (ret) {
			log_debug("cannot reset fb offsets (%d): %m", errno);
			return -EFAULT;
		}
	}

	if (disp->flags & DISPLAY_DBUF)
		log_debug("enabling double buffering");
	else
		log_debug("disabling double buffering");

	ret = refresh_info(disp);
	if (ret)
		return ret;

	/* We require TRUECOLOR mode here. That is, each pixel has a color value
	 * that is split into rgba values that we can set directly. Other visual
	 * modes like pseudocolor or direct-color do not provide this. As I have
	 * never seen a device that does not support TRUECOLOR, I think we can
	 * ignore them here. */
	if (finfo->visual != FB_VISUAL_TRUECOLOR ||
	    vinfo->bits_per_pixel != 32) {
		for (i = 0; depths[i]; ++i) {
			vinfo->bits_per_pixel = depths[i];
			vinfo->activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;

			ret = ioctl(fbdev->fd, FBIOPUT_VSCREENINFO,
				    vinfo);
			if (ret < 0)
				continue;

			ret = refresh_info(disp);
			if (ret)
				return ret;

			if (finfo->visual == FB_VISUAL_TRUECOLOR)
				break;
		}
	}

	if (vinfo->xres_virtual < vinfo->xres ||
	    (disp->flags & DISPLAY_DBUF &&
	     vinfo->yres_virtual < vinfo->yres * 2) ||
	    vinfo->yres_virtual < vinfo->yres) {
		log_warning("device %s has weird virtual buffer sizes (%d %d %d %d)",
			    fbdev->node, vinfo->xres, vinfo->xres_virtual,
			    vinfo->yres, vinfo->yres_virtual);
	}

	if (vinfo->bits_per_pixel != 32 &&
	    vinfo->bits_per_pixel != 16) {
		log_error("device %s does not support 16/32 bpp but: %u",
			  fbdev->node, vinfo->bits_per_pixel);
		return -EFAULT;
	}

	if (finfo->visual != FB_VISUAL_TRUECOLOR) {
		log_error("device %s does not support true-color",
			  fbdev->node);
		return -EFAULT;
	}

	if (vinfo->red.length > 8 ||
	    vinfo->green.length > 8 ||
	    vinfo->blue.length > 8) {
		log_error("device %s uses unusual color-ranges",
			  fbdev->node);
		return -EFAULT;
	}

	log_info("activating display %s to %ux%u %u bpp", fbdev->node,
		 vinfo->xres, vinfo->yres, vinfo->bits_per_pixel);

	/* calculate monitor rate, default is 60 Hz */
	quot = (vinfo->upper_margin + vinfo->lower_margin + vinfo->yres);
	quot *= (vinfo->left_margin + vinfo->right_margin + vinfo->xres);
	quot *= vinfo->pixclock;
	if (quot) {
		fbdev->rate = 1000000000000000LLU / quot;
	} else {
		fbdev->rate = 60 * 1000;
		log_warning("cannot read monitor refresh rate, forcing 60 Hz");
	}

	if (fbdev->rate == 0) {
		log_warning("monitor refresh rate is 0 Hz, forcing it to 1 Hz");
		fbdev->rate = 1;
	} else if (fbdev->rate > 200000) {
		log_warning("monitor refresh rate is >200 Hz (%u Hz), forcing it to 200 Hz",
			    fbdev->rate / 1000);
		fbdev->rate = 200000;
	}

	val = 1000000 / fbdev->rate;
	display_set_vblank_timer(disp, val);
	log_debug("vblank timer: %u ms, monitor refresh rate: %u Hz", val,
		  fbdev->rate / 1000);

	len = finfo->line_length * vinfo->yres;
	if (disp->flags & DISPLAY_DBUF)
		len *= 2;

	fbdev->map = mmap(0, len, PROT_WRITE, MAP_SHARED,
			       fbdev->fd, 0);
	if (fbdev->map == MAP_FAILED) {
		log_error("cannot mmap device %s (%d): %m", fbdev->node,
			  errno);
		return -EFAULT;
	}

	memset(fbdev->map, 0, len);
	fbdev->xres = vinfo->xres;
	fbdev->yres = vinfo->yres;
	fbdev->len = len;
	fbdev->stride = finfo->line_length;
	fbdev->bufid = 0;
	fbdev->Bpp = vinfo->bits_per_pixel / 8;
	fbdev->off_r = vinfo->red.offset;
	fbdev->len_r = vinfo->red.length;
	fbdev->off_g = vinfo->green.offset;
	fbdev->len_g = vinfo->green.length;
	fbdev->off_b = vinfo->blue.offset;
	fbdev->len_b = vinfo->blue.length;
	fbdev->dither_r = 0;
	fbdev->dither_g = 0;
	fbdev->dither_b = 0;
	fbdev->xrgb32 = false;
	if (fbdev->len_r == 8 &&
	    fbdev->len_g == 8 &&
	    fbdev->len_b == 8 &&
	    fbdev->off_r == 16 &&
	    fbdev->off_g ==  8 &&
	    fbdev->off_b ==  0 &&
	    fbdev->Bpp == 4)
		fbdev->xrgb32 = true;

	/* TODO: make dithering configurable */
	disp->flags |= DISPLAY_DITHERING;

	if (!disp->current_mode) {
		ret = mode_new(&m, &fbdev_mode_ops);
		if (ret) {
			munmap(fbdev->map, fbdev->len);
			return ret;
		}
		m->next = disp->modes;
		disp->modes = m;

		fbdev_mode->width = fbdev->xres;
		fbdev_mode->height = fbdev->yres;
		disp->current_mode = disp->modes;
	}

	disp->flags |= DISPLAY_ONLINE;
	return 0;
}

static int display_activate(struct uterm_display *disp, struct uterm_mode *mode)
{
	return display_activate_force(disp, mode, false);
}

static void display_deactivate_force(struct uterm_display *disp, bool force)
{
	struct fbdev_display *fbdev = disp->data;

	if (!disp->video || !(disp->flags & DISPLAY_ONLINE))
		return;

	log_info("deactivating device %s", fbdev->node);

	if (!force) {
		uterm_mode_unref(disp->current_mode);
		disp->modes = NULL;
		disp->current_mode = NULL;
	}
	memset(fbdev->map, 0, fbdev->len);
	munmap(fbdev->map, fbdev->len);

	if (!force)
		disp->flags &= ~DISPLAY_ONLINE;
}

static void display_deactivate(struct uterm_display *disp)
{
	return display_deactivate_force(disp, false);
}

static int display_set_dpms(struct uterm_display *disp, int state)
{
	int set, ret;
	struct fbdev_display *fbdev = disp->data;

	if (!disp->video || !(disp->flags & DISPLAY_ONLINE))
		return -EINVAL;

	switch (state) {
	case UTERM_DPMS_ON:
		set = FB_BLANK_UNBLANK;
		break;
	case UTERM_DPMS_STANDBY:
		set = FB_BLANK_NORMAL;
		break;
	case UTERM_DPMS_SUSPEND:
		set = FB_BLANK_NORMAL;
		break;
	case UTERM_DPMS_OFF:
		set = FB_BLANK_POWERDOWN;
		break;
	default:
		return -EINVAL;
	}

	log_info("setting DPMS of device %p to %s", fbdev->node,
		 uterm_dpms_to_name(state));

	ret = ioctl(fbdev->fd, FBIOBLANK, set);
	if (ret) {
		log_error("cannot set DPMS on %s (%d): %m", fbdev->node,
			  errno);
		return -EFAULT;
	}

	disp->dpms = state;
	return 0;
}

static int display_swap(struct uterm_display *disp)
{
	struct fb_var_screeninfo *vinfo;
	int ret;
	struct fbdev_display *fbdev = disp->data;

	if (!disp->video || !video_is_awake(disp->video))
		return -EINVAL;
	if (!(disp->flags & DISPLAY_ONLINE))
		return -EINVAL;

	if (!(disp->flags & DISPLAY_DBUF)) {
		return display_schedule_vblank_timer(disp);
	}

	vinfo = &fbdev->vinfo;
	vinfo->activate = FB_ACTIVATE_VBL;

	if (!fbdev->bufid)
		vinfo->yoffset = fbdev->yres;
	else
		vinfo->yoffset = 0;

	ret = ioctl(fbdev->fd, FBIOPUT_VSCREENINFO, vinfo);
	if (ret) {
		log_warning("cannot swap buffers on %s (%d): %m",
			    fbdev->node, errno);
		return -EFAULT;
	}

	fbdev->bufid ^= 1;
	return display_schedule_vblank_timer(disp);
}

static const struct display_ops fbdev_display_ops = {
	.init = NULL,
	.destroy = NULL,
	.activate = display_activate,
	.deactivate = display_deactivate,
	.set_dpms = display_set_dpms,
	.use = NULL,
	.swap = display_swap,
	.blit = uterm_fbdev_display_blit,
	.blend = uterm_fbdev_display_blend,
	.blendv = uterm_fbdev_display_fake_blendv,
	.fake_blendv = uterm_fbdev_display_fake_blendv,
	.fill = uterm_fbdev_display_fill,
};

static void intro_idle_event(struct ev_eloop *eloop, void *unused, void *data)
{
	struct uterm_display *disp = data;
	struct fbdev_display *fbdev = disp->data;

	if (!fbdev->pending_intro)
		return;

	fbdev->pending_intro = false;
	ev_eloop_unregister_idle_cb(eloop, intro_idle_event, disp);

	if (!disp->video)
		return;

	VIDEO_CB(disp->video, disp, UTERM_NEW);
}

static int video_init(struct uterm_video *video, const char *node)
{
	int ret;
	struct uterm_display *disp;
	struct fbdev_display *fbdev;

	fbdev = malloc(sizeof(*fbdev));
	if (!fbdev)
		return -ENOMEM;
	memset(fbdev, 0, sizeof(*fbdev));

	ret = display_new(&disp, &fbdev_display_ops, video);
	if (ret)
		goto err_fbdev;
	disp->data = fbdev;

	ret = ev_eloop_register_idle_cb(video->eloop, intro_idle_event, disp);
	if (ret) {
		log_error("cannot register idle event: %d", ret);
		goto err_free;
	}
	fbdev->pending_intro = true;

	fbdev->node = strdup(node);
	if (!fbdev->node) {
		log_err("cannot dup node name");
		ret = -ENOMEM;
		goto err_idle;
	}

	fbdev->fd = open(node, O_RDWR | O_CLOEXEC);
	if (fbdev->fd < 0) {
		log_err("cannot open %s (%d): %m", node, errno);
		ret = -EFAULT;
		goto err_node;
	}

	disp->dpms = UTERM_DPMS_UNKNOWN;
	video->displays = disp;

	log_info("new device on %s", fbdev->node);
	return 0;

err_node:
	free(fbdev->node);
err_idle:
	ev_eloop_register_idle_cb(video->eloop, intro_idle_event, disp);
err_free:
	uterm_display_unref(disp);
err_fbdev:
	free(fbdev);
	return ret;
}

static void video_destroy(struct uterm_video *video)
{
	struct uterm_display *disp;
	struct fbdev_display *fbdev;

	log_info("free device %p", video);
	disp = video->displays;
	video->displays = disp->next;
	fbdev = disp->data;

	if (fbdev->pending_intro)
		ev_eloop_unregister_idle_cb(video->eloop, intro_idle_event,
					    disp);
	else
		VIDEO_CB(video, disp, UTERM_GONE);

	close(fbdev->fd);
	free(fbdev->node);
	free(fbdev);
	uterm_display_unref(disp);
}

static void video_sleep(struct uterm_video *video)
{
	if (!(video->flags & VIDEO_AWAKE))
		return;

	display_deactivate_force(video->displays, true);
	video->flags &= ~VIDEO_AWAKE;
}

static int video_wake_up(struct uterm_video *video)
{
	int ret;

	if (video->flags & VIDEO_AWAKE)
		return 0;

	video->flags |= VIDEO_AWAKE;
	if (video->displays->flags & DISPLAY_ONLINE) {
		ret = display_activate_force(video->displays, NULL, true);
		if (ret) {
			video->flags &= ~VIDEO_AWAKE;
			return ret;
		}
	}

	return 0;
}

static const struct video_ops fbdev_video_ops = {
	.init = video_init,
	.destroy = video_destroy,
	.segfault = NULL, /* TODO */
	.use = NULL,
	.poll = NULL,
	.sleep = video_sleep,
	.wake_up = video_wake_up,
};

static const struct uterm_video_module fbdev_module = {
	.ops = &fbdev_video_ops,
};

const struct uterm_video_module *UTERM_VIDEO_FBDEV = &fbdev_module;