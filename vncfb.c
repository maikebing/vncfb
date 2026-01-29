// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fb.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/mm.h>

#define DRV_NAME "vncfb"
#define VNCFB_IOC_MAGIC     0xF5
#define VNCFB_IOC_GET_GEOM  _IOR(VNCFB_IOC_MAGIC, 0, struct vncfb_geom)
#define VNCFB_IOC_GET_DIRTY _IOWR(VNCFB_IOC_MAGIC, 1, struct vncfb_dirty)

struct vncfb_geom {
	__u32 width;
	__u32 height;
	__u32 bpp;
	__u32 line_length;
	__u64 vram_size;
};

struct vncfb_dirty {
	__u32 x0, y0, x1, y1; /* inclusive x0/y0, exclusive x1/y1; 如果无脏区则 x0==x1==0 */
};

static int fb_width = 1024;
static int fb_height = 768;
static int fb_bpp = 32; /* 16/24/32 supported */
static char fb_name[16] = DRV_NAME;

module_param(fb_width, int, 0644);
MODULE_PARM_DESC(fb_width, "Framebuffer width (pixels)");
module_param(fb_height, int, 0644);
MODULE_PARM_DESC(fb_height, "Framebuffer height (pixels)");
module_param(fb_bpp, int, 0644);
MODULE_PARM_DESC(fb_bpp, "Bits per pixel (16/24/32)");
module_param_string(fb_name, fb_name, sizeof(fb_name), 0644);
MODULE_PARM_DESC(fb_name, "fb fix.id (visible via /dev/fbX)");

struct vncfb_par {
	void *vram;
	size_t vram_size;
	u32 width, height, bpp, line_length;

	spinlock_t dirty_lock;
	bool dirty_valid;
	u32 dx0, dy0, dx1, dy1;

	struct fb_deferred_io dfx;
};

static struct fb_info *vncfb_info;

/* helpers */
static int vncfb_bytespp(u32 bpp)
{
	switch (bpp) {
	case 16: return 2; /* RGB565 */
	case 24: return 3; /* RGB888 */
	case 32: return 4; /* XRGB8888 */
	default: return -EINVAL;
	}
}

static int vncfb_alloc_vram(struct vncfb_par *par, u32 w, u32 h, u32 bpp)
{
	int bpp_bytes = vncfb_bytespp(bpp);
	size_t line_length;
	size_t sz;
	void *new;

	if (bpp_bytes < 0)
		return -EINVAL;

	line_length = w * bpp_bytes; /* 不强制 4 字节对齐 */
	sz = (size_t)line_length * h;

	new = vzalloc(sz);
	if (!new)
		return -ENOMEM;

	if (par->vram)
		vfree(par->vram);

	par->vram = new;
	par->vram_size = sz;
	par->width = w;
	par->height = h;
	par->bpp = bpp;
	par->line_length = line_length;
	par->dirty_valid = false;

	return 0;
}

static void vncfb_setup_channels(struct fb_var_screeninfo *var)
{
	switch (var->bits_per_pixel) {
	case 16: /* RGB565 */
		var->red.offset   = 11; var->red.length   = 5;
		var->green.offset = 5; var->green.length = 6;
		var->blue.offset  = 0; var->blue.length  = 5;
		var->transp.offset = 0; var->transp.length = 0;
		break;
	case 24: /* RGB888 */
		var->red.offset   = 16; var->red.length   = 8;
		var->green.offset = 8; var->green.length = 8;
		var->blue.offset  = 0; var->blue.length  = 8;
		var->transp.offset = 0; var->transp.length = 0;
		break;
	case 32: /* XRGB8888 */
		var->red.offset   = 16; var->red.length   = 8;
		var->green.offset = 8; var->green.length = 8;
		var->blue.offset  = 0; var->blue.length  = 8;
		var->transp.offset = 24; var->transp.length = 8;
		break;
	}
}

static void vncfb_update_fix_var(struct fb_info *info)
{
	struct vncfb_par *par = info->par;

	/* fix */
	strlcpy(info->fix.id, fb_name, sizeof(info->fix.id));
	info->fix.type = FB_TYPE_PACKED_PIXELS;
	info->fix.visual = FB_VISUAL_TRUECOLOR;
	info->fix.line_length = par->line_length;
	info->fix.smem_len = par->vram_size;
	info->fix.smem_start = 0; /* vmalloc */

	/* var */
	info->var.xres = par->width;
	info->var.yres = par->height;
	info->var.xres_virtual = par->width;
	info->var.yres_virtual = par->height;
	info->var.bits_per_pixel = par->bpp;
	vncfb_setup_channels(&info->var);
	info->var.activate = FB_ACTIVATE_NOW;

	info->screen_base = par->vram;
	info->screen_size = par->vram_size;
}

/* fbops */
static int vncfb_setcolreg(unsigned regno,
	unsigned red,
	unsigned green,
	unsigned blue,
	unsigned transp,
	struct fb_info *info)
{
	if (regno > 255)
		return 1;
	return 0;
}

static int vncfb_ioctl(struct fb_info *info, unsigned int cmd, unsigned long arg)
{
	struct vncfb_par *par = info->par;
	int ret = 0;

	switch (cmd) {
	case VNCFB_IOC_GET_GEOM: {
			struct vncfb_geom g = {
				.width = info->var.xres,
				.height = info->var.yres,
				.bpp = info->var.bits_per_pixel,
				.line_length = info->fix.line_length,
				.vram_size = par->vram_size,
			};
			if (copy_to_user((void __user *)arg, &g, sizeof(g)))
				ret = -EFAULT;
			break;
		}
	case VNCFB_IOC_GET_DIRTY: {
			struct vncfb_dirty d = { 0 };
			unsigned long flags;
			spin_lock_irqsave(&par->dirty_lock, flags);
			if (par->dirty_valid) {
				d.x0 = par->dx0; d.y0 = par->dy0; d.x1 = par->dx1; d.y1 = par->dy1;
				par->dirty_valid = false;
			}
			spin_unlock_irqrestore(&par->dirty_lock, flags);
			if (copy_to_user((void __user *)arg, &d, sizeof(d)))
				ret = -EFAULT;
			break;
		}
	default:
		ret = -ENOIOCTLCMD;
	}
	return ret;
}

static int vncfb_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
	struct vncfb_par *par = info->par;
	int ret = remap_vmalloc_range(vma, par->vram, 0);
	if (ret)
		pr_err(DRV_NAME ": remap_vmalloc_range failed: %d\n", ret);
	return ret;
}

static int vncfb_set_par(struct fb_info *info)
{
	struct vncfb_par *par = info->par;
	u32 w = info->var.xres;
	u32 h = info->var.yres;
	u32 bpp = info->var.bits_per_pixel;
	int ret;

	if (vncfb_bytespp(bpp) < 0)
		return -EINVAL;

	ret = vncfb_alloc_vram(par, w, h, bpp);
	if (ret)
		return ret;

	vncfb_update_fix_var(info);
	pr_info(DRV_NAME ": mode set %ux%u@%u\n", w, h, bpp);
	return 0;
}

static int vncfb_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
	if (vncfb_bytespp(var->bits_per_pixel) < 0)
		return -EINVAL;
	if (var->xres == 0 || var->yres == 0)
		return -EINVAL;
	vncfb_setup_channels(var);
	return 0;
}

static struct fb_ops vncfb_ops = {
	.owner = THIS_MODULE,
	.fb_read = fb_sys_read,
	.fb_write = fb_sys_write,
	.fb_fillrect = sys_fillrect,
	.fb_copyarea = sys_copyarea,
	.fb_imageblit = sys_imageblit,
	.fb_mmap = vncfb_mmap,
	.fb_setcolreg = vncfb_setcolreg,
	.fb_ioctl = vncfb_ioctl,
#ifdef CONFIG_COMPAT
	.fb_compat_ioctl = vncfb_ioctl,
#endif
	.fb_check_var = vncfb_check_var,
	.fb_set_par = vncfb_set_par,
};

/* deferred IO */
static void vncfb_deferred_io(struct fb_info *info, struct list_head *pagelist)
{
	struct vncfb_par *par = info->par;
	struct page *page;
	unsigned long flags;
	u32 minx = par->width, miny = par->height, maxx = 0, maxy = 0;
	bool any = false;
	int bpp_bytes = vncfb_bytespp(par->bpp);

	list_for_each_entry(page, pagelist, lru) {
		unsigned long offset = page->index << PAGE_SHIFT;
		u32 y = offset / par->line_length;
		u32 x_bytes = offset - y * par->line_length;
		u32 x = x_bytes / bpp_bytes;

		if (y >= par->height) continue;
		any = true;
		if (x < minx) minx = x;
		if (y < miny) miny = y;
		if (x + 1 > maxx) maxx = x + 1;
		if (y + 1 > maxy) maxy = y + 1;
	}

	if (!any)
		return;

	spin_lock_irqsave(&par->dirty_lock, flags);
	if (!par->dirty_valid) {
		par->dx0 = minx; par->dy0 = miny; par->dx1 = maxx; par->dy1 = maxy;
		par->dirty_valid = true;
	}
	else {
		if (minx < par->dx0) par->dx0 = minx;
		if (miny < par->dy0) par->dy0 = miny;
		if (maxx > par->dx1) par->dx1 = maxx;
		if (maxy > par->dy1) par->dy1 = maxy;
	}
	spin_unlock_irqrestore(&par->dirty_lock, flags);
}

/* init/exit */
static int __init vncfb_init(void)
{
	struct vncfb_par *par;
	int ret;

	if (vncfb_bytespp(fb_bpp) < 0) {
		pr_warn(DRV_NAME ": bpp %d unsupported, forcing to 32\n", fb_bpp);
		fb_bpp = 32;
	}

	vncfb_info = framebuffer_alloc(sizeof(struct vncfb_par), NULL);
	if (!vncfb_info)
		return -ENOMEM;

	par = vncfb_info->par;
	spin_lock_init(&par->dirty_lock);

	ret = vncfb_alloc_vram(par, fb_width, fb_height, fb_bpp);
	if (ret)
		goto err_alloc;

	vncfb_update_fix_var(vncfb_info);

	vncfb_info->fbops = &vncfb_ops;
	/* fb flags */
#ifdef FBINFO_FLAG_DEFAULT
	vncfb_info->flags = FBINFO_FLAG_DEFAULT;
#elif defined(FBINFO_DEFAULT)
	vncfb_info->flags = FBINFO_DEFAULT;
#else
	vncfb_info->flags = 0;
#endif
	vncfb_info->screen_base = par->vram;
	vncfb_info->screen_size = par->vram_size;

	par->dfx.delay = HZ / 30; /* ~33ms，可调 */
	par->dfx.deferred_io = vncfb_deferred_io;
	vncfb_info->fbdefio = &par->dfx;
	fb_deferred_io_init(vncfb_info);

	memset(par->vram, 0x10, par->vram_size);

	ret = register_framebuffer(vncfb_info);
	if (ret < 0) {
		pr_err(DRV_NAME ": register_framebuffer failed: %d\n", ret);
		goto err_vfree;
	}

	pr_info(DRV_NAME ": registered /dev/fb%d (%s) %ux%u@%ubpp vram=%zu bytes\n",
		vncfb_info->node,
		vncfb_info->fix.id,
		par->width,
		par->height,
		par->bpp,
		par->vram_size);
	return 0;

err_vfree:
	vfree(par->vram);
err_alloc:
	framebuffer_release(vncfb_info);
	return ret;
}

static void __exit vncfb_exit(void)
{
	struct vncfb_par *par;

	if (!vncfb_info)
		return;
	par = vncfb_info->par;

	fb_deferred_io_cleanup(vncfb_info);
	unregister_framebuffer(vncfb_info);
	vfree(par->vram);
	framebuffer_release(vncfb_info);
	pr_info(DRV_NAME ": unloaded\n");
}

module_init(vncfb_init);
module_exit(vncfb_exit);

MODULE_AUTHOR("you");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("vmalloc-backed fbdev driver for VNC with dynamic mode, deferred IO, dirty ioctl, 16/24/32bpp");