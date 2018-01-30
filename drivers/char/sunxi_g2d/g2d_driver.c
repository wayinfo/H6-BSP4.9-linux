/*
 * Allwinner SoCs g2d driver.
 *
 * Copyright (C) 2016 Allwinner.
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include "g2d_driver_i.h"
#include <linux/g2d_driver.h>

/* alloc based on 4K byte */
#define G2D_BYTE_ALIGN(x) (((x + (4*1024-1)) >> 12) << 12)
static struct info_mem g2d_mem[MAX_G2D_MEM_INDEX];
static int g2d_mem_sel;
static enum g2d_scan_order scan_order;
static struct mutex global_lock;

static struct class *g2d_class;
static struct cdev *g2d_cdev;
static dev_t devid;
static struct device *g2d_dev;
static struct device *dmabuf_dev;
__g2d_drv_t g2d_ext_hd;
__g2d_info_t para;

u32 dbg_info;

#if (defined CONFIG_ARCH_SUN8IW15P1)
#define USE_DMA_BUF
#endif

struct dmabuf_item {
	struct list_head list;
	int fd;
	struct dma_buf *buf;
	struct dma_buf_attachment *attachment;
	struct sg_table *sgt;
	dma_addr_t dma_addr;
	unsigned long long id;
};

#if !defined(CONFIG_OF)
static struct resource g2d_resource[2] = {

	[0] = {
	       .start = SUNXI_MP_PBASE,
	       .end = SUNXI_MP_PBASE + 0x000fffff,
	       .flags = IORESOURCE_MEM,
	       },
	[1] = {
	       .start = INTC_IRQNO_DE_MIX,
	       .end = INTC_IRQNO_DE_MIX,
	       .flags = IORESOURCE_IRQ,
	       },
};
#endif

static ssize_t g2d_debug_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "debug=%d\n", dbg_info);
}

static ssize_t g2d_debug_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	if (strncasecmp(buf, "1", 1) == 0)
		dbg_info = 1;
	else if (strncasecmp(buf, "0", 1) == 0)
		dbg_info = 0;
	else
		WARNING("Error input!\n");

	return count;
}

static DEVICE_ATTR(debug, 0660,
		   g2d_debug_show, g2d_debug_store);

static struct attribute *g2d_attributes[] = {
	&dev_attr_debug.attr,
	NULL
};

static struct attribute_group g2d_attribute_group = {
	.name = "attr",
	.attrs = g2d_attributes
};

#ifdef USE_DMA_BUF
static int g2d_dma_map(int fd, struct dmabuf_item *item)
{
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attachment;
	struct sg_table *sgt, *sgt_bak;
	struct scatterlist *sgl, *sgl_bak;
	s32 sg_count = 0;
	int ret = -1;
	int i;

	if (fd < 0) {
		pr_err("dma_buf_id(%d) is invalid\n", fd);
		goto exit;
	}
	dmabuf = dma_buf_get(fd);
	if (IS_ERR(dmabuf)) {
		pr_err("dma_buf_get failed\n");
		goto exit;
	}

	attachment = dma_buf_attach(dmabuf, dmabuf_dev);
	if (IS_ERR(attachment)) {
		pr_err("dma_buf_attach failed\n");
		goto err_buf_put;
	}
	sgt = dma_buf_map_attachment(attachment, DMA_FROM_DEVICE);
	if (IS_ERR_OR_NULL(sgt)) {
		pr_err("dma_buf_map_attachment failed\n");
		goto err_buf_detach;
	}

	/* create a private sgtable base on the given dmabuf */
	sgt_bak = kmalloc(sizeof(struct sg_table), GFP_KERNEL | __GFP_ZERO);
	if (sgt_bak == NULL) {
		pr_err("alloc sgt fail\n");
		goto err_buf_unmap;
	}
	ret = sg_alloc_table(sgt_bak, sgt->nents, GFP_KERNEL);
	if (ret != 0) {
		pr_err("alloc sgt fail\n");
		goto err_kfree;
	}
	sgl_bak = sgt_bak->sgl;
	for_each_sg(sgt->sgl, sgl, sgt->nents, i)  {
		sg_set_page(sgl_bak, sg_page(sgl), sgl->length, sgl->offset);
		sgl_bak = sg_next(sgl_bak);
	}

	sg_count = dma_map_sg_attrs(dmabuf_dev, sgt_bak->sgl,
			      sgt_bak->nents, DMA_FROM_DEVICE,
			      DMA_ATTR_SKIP_CPU_SYNC);

	if (sg_count != 1) {
		pr_err("dma_map_sg failed:%d\n", sg_count);
		goto err_sgt_free;
	}

	item->fd = fd;
	item->buf = dmabuf;
	item->sgt = sgt_bak;
	item->attachment = attachment;
	item->dma_addr = sg_dma_address(sgt_bak->sgl);
	ret = 0;

	goto exit;

err_sgt_free:
	sg_free_table(sgt_bak);
err_kfree:
	kfree(sgt_bak);
err_buf_unmap:
	/* unmap attachment sgt, not sgt_bak, because it's not alloc yet! */
	dma_buf_unmap_attachment(attachment, sgt, DMA_FROM_DEVICE);
err_buf_detach:
	dma_buf_detach(dmabuf, attachment);
err_buf_put:
	dma_buf_put(dmabuf);
exit:
	return ret;
}

static void g2d_dma_unmap(struct dmabuf_item *item)
{

	dma_unmap_sg_attrs(dmabuf_dev, item->sgt->sgl,
			      item->sgt->nents, DMA_FROM_DEVICE,
			      DMA_ATTR_SKIP_CPU_SYNC);
	dma_buf_unmap_attachment(item->attachment, item->sgt, DMA_FROM_DEVICE);
	sg_free_table(item->sgt);
	kfree(item->sgt);
	dma_buf_detach(item->buf, item->attachment);
	dma_buf_put(item->buf);
}

static struct g2d_format_attr fmt_attr_tbl[] = {
	/*
	      format                            bits
						   hor_rsample(u,v)
							  ver_rsample(u,v)
								uvc
								   interleave
								       factor
									  div
	 */
	{ G2D_FORMAT_ARGB8888, 8,  1, 1, 1, 1, 0, 1, 4, 1},
	{ G2D_FORMAT_ABGR8888, 8,  1, 1, 1, 1, 0, 1, 4, 1},
	{ G2D_FORMAT_RGBA8888, 8,  1, 1, 1, 1, 0, 1, 4, 1},
	{ G2D_FORMAT_BGRA8888, 8,  1, 1, 1, 1, 0, 1, 4, 1},
	{ G2D_FORMAT_XRGB8888, 8,  1, 1, 1, 1, 0, 1, 4, 1},
	{ G2D_FORMAT_XBGR8888, 8,  1, 1, 1, 1, 0, 1, 4, 1},
	{ G2D_FORMAT_RGBX8888, 8,  1, 1, 1, 1, 0, 1, 4, 1},
	{ G2D_FORMAT_BGRX8888, 8,  1, 1, 1, 1, 0, 1, 4, 1},
	{ G2D_FORMAT_RGB888, 8,  1, 1, 1, 1, 0, 1, 3, 1},
	{ G2D_FORMAT_BGR888, 8,  1, 1, 1, 1, 0, 1, 3, 1},
	{ G2D_FORMAT_RGB565, 8,  1, 1, 1, 1, 0, 1, 2, 1},
	{ G2D_FORMAT_BGR565, 8,  1, 1, 1, 1, 0, 1, 2, 1},
	{ G2D_FORMAT_ARGB4444, 8,  1, 1, 1, 1, 0, 1, 2, 1},
	{ G2D_FORMAT_ABGR4444, 8,  1, 1, 1, 1, 0, 1, 2, 1},
	{ G2D_FORMAT_RGBA4444, 8,  1, 1, 1, 1, 0, 1, 2, 1},
	{ G2D_FORMAT_BGRA4444, 8,  1, 1, 1, 1, 0, 1, 2, 1},
	{ G2D_FORMAT_ARGB1555, 8,  1, 1, 1, 1, 0, 1, 2, 1},
	{ G2D_FORMAT_ABGR1555, 8,  1, 1, 1, 1, 0, 1, 2, 1},
	{ G2D_FORMAT_RGBA5551, 8,  1, 1, 1, 1, 0, 1, 2, 1},
	{ G2D_FORMAT_BGRA5551, 8,  1, 1, 1, 1, 0, 1, 2, 1},
	{ G2D_FORMAT_ARGB2101010, 10, 1, 1, 1, 1, 0, 1, 4, 1},
	{ G2D_FORMAT_ABGR2101010, 10, 1, 1, 1, 1, 0, 1, 4, 1},
	{ G2D_FORMAT_RGBA1010102, 10, 1, 1, 1, 1, 0, 1, 4, 1},
	{ G2D_FORMAT_BGRA1010102, 10, 1, 1, 1, 1, 0, 1, 4, 1},
	{ G2D_FORMAT_IYUV422_V0Y1U0Y0, 8,  1, 1, 1, 1, 0, 1, 2, 1},
	{ G2D_FORMAT_IYUV422_Y1V0Y0U0, 8,  1, 1, 1, 1, 0, 1, 2, 1},
	{ G2D_FORMAT_IYUV422_U0Y1V0Y0, 8,  1, 1, 1, 1, 0, 1, 2, 1},
	{ G2D_FORMAT_IYUV422_Y1U0Y0V0, 8,  1, 1, 1, 1, 0, 1, 2, 1},
	{ G2D_FORMAT_YUV422_PLANAR, 8,  2, 2, 1, 1, 0, 0, 2, 1},
	{ G2D_FORMAT_YUV420_PLANAR, 8,  2, 2, 2, 2, 0, 0, 3, 2},
	{ G2D_FORMAT_YUV411_PLANAR, 8,  4, 4, 1, 1, 0, 0, 3, 2},
	{ G2D_FORMAT_YUV422UVC_U1V1U0V0, 8,  2, 2, 1, 1, 1, 0, 2, 1},
	{ G2D_FORMAT_YUV422UVC_V1U1V0U0, 8,  2, 2, 1, 1, 1, 0, 2, 1},
	{ G2D_FORMAT_YUV420UVC_U1V1U0V0, 8,  2, 2, 2, 2, 1, 0, 3, 2},
	{ G2D_FORMAT_YUV420UVC_V1U1V0U0, 8,  2, 2, 2, 2, 1, 0, 3, 2},
	{ G2D_FORMAT_YUV411UVC_U1V1U0V0, 8,  4, 4, 1, 1, 1, 0, 3, 2},
	{ G2D_FORMAT_YUV411UVC_V1U1V0U0, 8,  4, 4, 1, 1, 1, 0, 3, 2},
	{ G2D_FORMAT_Y8, 8,  1, 1, 1, 1, 0, 0, 1, 1},
	{ G2D_FORMAT_YVU10_444, 10, 1, 1, 1, 1, 0, 1, 4, 1},
	{ G2D_FORMAT_YUV10_444, 10, 1, 1, 1, 1, 0, 1, 4, 1},
	{ G2D_FORMAT_YVU10_P210, 10, 2, 2, 1, 1, 0, 0, 4, 1},
	{ G2D_FORMAT_YVU10_P010, 10, 2, 2, 2, 2, 0, 0, 3, 1},
};

s32 g2d_set_info(g2d_image_enh *g2d_img, struct dmabuf_item *item)
{
	s32 ret = -1;
	u32 i = 0;
	u32 len = ARRAY_SIZE(fmt_attr_tbl);
	u32 y_width, y_height, u_width, u_height;
	u32 y_pitch, u_pitch;
	u32 y_size, u_size;

	g2d_img->laddr[0] = item->dma_addr;

	if (g2d_img->format >= G2D_FORMAT_MAX) {
		pr_err("%s, format 0x%x is out of range\n", __func__,
			g2d_img->format);
		goto exit;
	}

	for (i = 0; i < len; ++i) {

		if (fmt_attr_tbl[i].format == g2d_img->format) {
			y_width = g2d_img->clip_rect.w;
			y_height = g2d_img->clip_rect.h;
			u_width = y_width/fmt_attr_tbl[i].hor_rsample_u;
			u_height = y_height/fmt_attr_tbl[i].ver_rsample_u;

			y_pitch = y_width;
			u_pitch = u_width * (fmt_attr_tbl[i].uvc + 1);

			y_size = y_pitch * y_height;
			u_size = u_pitch * u_height;
			g2d_img->laddr[1] = g2d_img->laddr[0] + y_size;
			g2d_img->laddr[2] = g2d_img->laddr[0] + y_size + u_size;

			ret = 0;
			break;
		}
	}
	if (ret != 0)
		pr_err("%s, format 0x%x is invalid\n", __func__,
			g2d_img->format);
exit:
	return ret;

}
#endif

__s32 drv_g2d_init(void)
{
	g2d_init_para init_para;

	DBG("drv_g2d_init\n");
	init_para.g2d_base = (unsigned long) para.io;
	memset(&g2d_ext_hd, 0, sizeof(__g2d_drv_t));
	init_waitqueue_head(&g2d_ext_hd.queue);
	g2d_init(&init_para);

	return 0;
}

void *g2d_malloc(__u32 bytes_num, __u32 *phy_addr)
{
	void *address = NULL;

#if defined(CONFIG_ION_SUNXI)
	u32 actual_bytes;

	if (bytes_num != 0) {
		actual_bytes = PAGE_ALIGN(bytes_num);

		address = dma_alloc_coherent(para.dev, actual_bytes,
					     (dma_addr_t *) phy_addr,
					     GFP_KERNEL);
		if (address) {
			DBG("dma_alloc_coherent ok, address=0x%p, size=0x%x\n",
			    (void *)(*(unsigned long *)phy_addr), bytes_num);
			return address;
		}
		ERR("dma_alloc_coherent fail, size=0x%x\n", bytes_num);
		return NULL;
	}
	ERR("%s size is zero\n", __func__);
#else
	unsigned map_size = 0;
	struct page *page;

	if (bytes_num != 0) {
		map_size = PAGE_ALIGN(bytes_num);
		page = alloc_pages(GFP_KERNEL, get_order(map_size));
		if (page != NULL) {
			address = page_address(page);
			if (address == NULL) {
				free_pages((unsigned long)(page),
					   get_order(map_size));
				ERR("page_address fail!\n");
				return NULL;
			}
			*phy_addr = virt_to_phys(address);
			return address;
		}
		ERR("alloc_pages fail!\n");
		return NULL;
	}
	ERR("%s size is zero\n", __func__);
#endif

	return NULL;
}

void g2d_free(void *virt_addr, void *phy_addr, unsigned int size)
{
#if defined(CONFIG_ION_SUNXI)
	u32 actual_bytes;

	actual_bytes = PAGE_ALIGN(size);
	if (phy_addr && virt_addr)
		dma_free_coherent(para.dev, actual_bytes, virt_addr,
				  (dma_addr_t) phy_addr);
#else
	unsigned map_size = PAGE_ALIGN(size);
	unsigned page_size = map_size;

	if (virt_addr == NULL)
		return;

	free_pages((unsigned long)virt_addr, get_order(page_size));
#endif
}

__s32 g2d_get_free_mem_index(void)
{
	__u32 i = 0;

	for (i = 0; i < MAX_G2D_MEM_INDEX; i++) {
		if (g2d_mem[i].b_used == 0)
			return i;
	}
	return -1;
}

int g2d_mem_request(__u32 size)
{
	__s32 sel;
	unsigned long ret = 0;
	__u32 phy_addr;

	sel = g2d_get_free_mem_index();
	if (sel < 0) {
		ERR("g2d_get_free_mem_index fail!\n");
		return -EINVAL;
	}

	ret = (unsigned long)g2d_malloc(size, &phy_addr);
	if (ret != 0) {
		g2d_mem[sel].virt_addr = (void *)ret;
		memset(g2d_mem[sel].virt_addr, 0, size);
		g2d_mem[sel].phy_addr = phy_addr;
		g2d_mem[sel].mem_len = size;
		g2d_mem[sel].b_used = 1;

		INFO("map_g2d_memory[%d]: pa=%08lx va=%p size:%x\n", sel,
		     g2d_mem[sel].phy_addr, g2d_mem[sel].virt_addr, size);
		return sel;
	}
	ERR("fail to alloc reserved memory!\n");
	return -ENOMEM;
}

int g2d_mem_release(__u32 sel)
{
	if (g2d_mem[sel].b_used == 0) {
		ERR("mem not used in g2d_mem_release,%d\n", sel);
		return -EINVAL;
	}

	g2d_free((void *)g2d_mem[sel].virt_addr, (void *)g2d_mem[sel].phy_addr,
		 g2d_mem[sel].mem_len);
	memset(&g2d_mem[sel], 0, sizeof(struct info_mem));

	return 0;
}

int g2d_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long mypfn = vma->vm_pgoff;
	unsigned long vmsize = vma->vm_end - vma->vm_start;

	vma->vm_pgoff = 0;

	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	if (remap_pfn_range(vma, vma->vm_start, mypfn,
			    vmsize, vma->vm_page_prot))
		return -EAGAIN;

	return 0;
}

static int g2d_open(struct inode *inode, struct file *file)
{
	mutex_lock(&para.mutex);
	para.user_cnt++;
	if (para.user_cnt == 1) {
		if (para.clk)
			clk_prepare_enable(para.clk);
		para.opened = true;
#ifdef G2D_V2X_SUPPORT
		g2d_bsp_open();
#endif
	}

	mutex_unlock(&para.mutex);
	return 0;
}

static int g2d_release(struct inode *inode, struct file *file)
{
	mutex_lock(&para.mutex);
	para.user_cnt--;
	if (para.user_cnt == 0) {
		if (para.clk)
			clk_disable(para.clk);
		para.opened = false;
#ifdef G2D_V2X_SUPPORT
		g2d_bsp_close();
#endif
	}

	mutex_unlock(&para.mutex);

	mutex_lock(&global_lock);
	scan_order = G2D_SM_TDLR;
	mutex_unlock(&global_lock);

	return 0;
}

irqreturn_t g2d_handle_irq(int irq, void *dev_id)
{
#ifdef G2D_V2X_SUPPORT
	__u32 mixer_irq_flag, rot_irq_flag;

	mixer_irq_flag = mixer_irq_query();
	rot_irq_flag = rot_irq_query();
	if (mixer_irq_flag == 0 || rot_irq_flag == 0) {
		g2d_ext_hd.finish_flag = 1;
		wake_up(&g2d_ext_hd.queue);
	}
#else
	__u32 mod_irq_flag, cmd_irq_flag;

	mod_irq_flag = mixer_get_irq();
	cmd_irq_flag = mixer_get_irq0();
	if (mod_irq_flag & G2D_FINISH_IRQ) {
		mixer_clear_init();
		g2d_ext_hd.finish_flag = 1;
		wake_up(&g2d_ext_hd.queue);
	} else if (cmd_irq_flag & G2D_FINISH_IRQ) {
		mixer_clear_init0();
		g2d_ext_hd.finish_flag = 1;
		wake_up(&g2d_ext_hd.queue);
	}
#endif
	return IRQ_HANDLED;
}

int g2d_init(g2d_init_para *para)
{
	mixer_set_reg_base(para->g2d_base);

	return 0;
}

int g2d_exit(void)
{
	__u8 err = 0;

	return err;
}

int g2d_wait_cmd_finish(void)
{
	long timeout = 100;	/* 100ms */

	timeout = wait_event_timeout(g2d_ext_hd.queue,
				     g2d_ext_hd.finish_flag == 1,
				     msecs_to_jiffies(timeout));
	if (timeout == 0) {
#ifdef G2D_V2X_SUPPORT
		g2d_bsp_reset();
#else
		mixer_clear_init();
		mixer_clear_init0();
#endif
		pr_warn("G2D irq pending flag timeout\n");
		g2d_ext_hd.finish_flag = 1;
		wake_up(&g2d_ext_hd.queue);
		return -1;
	}
	g2d_ext_hd.finish_flag = 0;

	return 0;
}

int g2d_blit(g2d_blt *para)
{
	__s32 err = 0;
	__u32 tmp_w, tmp_h;

	if ((para->flag & G2D_BLT_ROTATE90) ||
			(para->flag & G2D_BLT_ROTATE270)) {
		tmp_w = para->src_rect.h;
		tmp_h = para->src_rect.w;
	} else {
		tmp_w = para->src_rect.w;
		tmp_h = para->src_rect.h;
	}
	/* check the parameter valid */
	if (((para->src_rect.x < 0) &&
	     ((-para->src_rect.x) > para->src_rect.w)) ||
	    ((para->src_rect.y < 0) &&
	     ((-para->src_rect.y) > para->src_rect.h)) ||
	    ((para->dst_x < 0) &&
	     ((-para->dst_x) > tmp_w)) ||
	    ((para->dst_y < 0) &&
	     ((-para->dst_y) > tmp_h)) ||
	    ((para->src_rect.x > 0) &&
	     (para->src_rect.x > para->src_image.w - 1)) ||
	    ((para->src_rect.y > 0) &&
	     (para->src_rect.y > para->src_image.h - 1)) ||
	    ((para->dst_x > 0) &&
	     (para->dst_x > para->dst_image.w - 1)) ||
	    ((para->dst_y > 0) && (para->dst_y > para->dst_image.h - 1))) {
		pr_warn("invalid blit parameter setting");
		return -EINVAL;
	}
	if (((para->src_rect.x < 0) &&
				((-para->src_rect.x) < para->src_rect.w))) {
		para->src_rect.w = para->src_rect.w + para->src_rect.x;
		para->src_rect.x = 0;
	} else if ((para->src_rect.x + para->src_rect.w)
			> para->src_image.w) {
		para->src_rect.w = para->src_image.w - para->src_rect.x;
	}
	if (((para->src_rect.y < 0) &&
				((-para->src_rect.y) < para->src_rect.h))) {
		para->src_rect.h = para->src_rect.h + para->src_rect.y;
		para->src_rect.y = 0;
	} else if ((para->src_rect.y + para->src_rect.h)
			> para->src_image.h) {
		para->src_rect.h = para->src_image.h - para->src_rect.y;
	}

	if (((para->dst_x < 0) && ((-para->dst_x) < tmp_w))) {
		para->src_rect.w = tmp_w + para->dst_x;
		para->src_rect.x = (-para->dst_x);
		para->dst_x = 0;
	} else if ((para->dst_x + tmp_w) > para->dst_image.w) {
		para->src_rect.w = para->dst_image.w - para->dst_x;
	}
	if (((para->dst_y < 0) && ((-para->dst_y) < tmp_h))) {
		para->src_rect.h = tmp_h + para->dst_y;
		para->src_rect.y = (-para->dst_y);
		para->dst_y = 0;
	} else if ((para->dst_y + tmp_h) > para->dst_image.h)
		para->src_rect.h = para->dst_image.h - para->dst_y;

	g2d_ext_hd.finish_flag = 0;

	/* Add support inverted order copy, however,
	 * hardware have a bug when reciving y coordinate,
	 * it use (y + height) rather than (y) on inverted
	 * order mode, so here adjust it before pass it to hardware.
	 */
	mutex_lock(&global_lock);
	if (scan_order > G2D_SM_TDRL)
		para->dst_y += para->src_rect.h;
	mutex_unlock(&global_lock);

	err = mixer_blt(para, scan_order);

	return err;
}

int g2d_fill(g2d_fillrect *para)
{
	__s32 err = 0;

	/* check the parameter valid */
	if (((para->dst_rect.x < 0) &&
	     ((-para->dst_rect.x) > para->dst_rect.w)) ||
	    ((para->dst_rect.y < 0) &&
	     ((-para->dst_rect.y) > para->dst_rect.h)) ||
	    ((para->dst_rect.x > 0) &&
	     (para->dst_rect.x > para->dst_image.w - 1)) ||
	    ((para->dst_rect.y > 0) &&
	     (para->dst_rect.y > para->dst_image.h - 1))) {
		pr_warn("invalid fillrect parameter setting");
		return -EINVAL;
	}
	if (((para->dst_rect.x < 0) &&
				((-para->dst_rect.x) < para->dst_rect.w))) {
		para->dst_rect.w = para->dst_rect.w + para->dst_rect.x;
		para->dst_rect.x = 0;
	} else if ((para->dst_rect.x + para->dst_rect.w)
			> para->dst_image.w) {
		para->dst_rect.w = para->dst_image.w - para->dst_rect.x;
	}
	if (((para->dst_rect.y < 0) &&
				((-para->dst_rect.y) < para->dst_rect.h))) {
		para->dst_rect.h = para->dst_rect.h + para->dst_rect.y;
		para->dst_rect.y = 0;
	} else if ((para->dst_rect.y + para->dst_rect.h)
			> para->dst_image.h)
		para->dst_rect.h = para->dst_image.h - para->dst_rect.y;

	g2d_ext_hd.finish_flag = 0;
	err = mixer_fillrectangle(para);

	return err;
}

int g2d_stretchblit(g2d_stretchblt *para)
{
	__s32 err = 0;

	/* check the parameter valid */
	if (((para->src_rect.x < 0) &&
	     ((-para->src_rect.x) > para->src_rect.w)) ||
	    ((para->src_rect.y < 0) &&
	     ((-para->src_rect.y) > para->src_rect.h)) ||
	    ((para->dst_rect.x < 0) &&
	     ((-para->dst_rect.x) > para->dst_rect.w)) ||
	    ((para->dst_rect.y < 0) &&
	     ((-para->dst_rect.y) > para->dst_rect.h)) ||
	    ((para->src_rect.x > 0) &&
	     (para->src_rect.x > para->src_image.w - 1)) ||
	    ((para->src_rect.y > 0) &&
	     (para->src_rect.y > para->src_image.h - 1)) ||
	    ((para->dst_rect.x > 0) &&
	     (para->dst_rect.x > para->dst_image.w - 1)) ||
	    ((para->dst_rect.y > 0) &&
	     (para->dst_rect.y > para->dst_image.h - 1))) {
		pr_warn("invalid stretchblit parameter setting");
		return -EINVAL;
	}
	if (((para->src_rect.x < 0) &&
				((-para->src_rect.x) < para->src_rect.w))) {
		para->src_rect.w = para->src_rect.w + para->src_rect.x;
		para->src_rect.x = 0;
	} else if ((para->src_rect.x + para->src_rect.w)
			> para->src_image.w) {
		para->src_rect.w = para->src_image.w - para->src_rect.x;
	}
	if (((para->src_rect.y < 0) &&
				((-para->src_rect.y) < para->src_rect.h))) {
		para->src_rect.h = para->src_rect.h + para->src_rect.y;
		para->src_rect.y = 0;
	} else if ((para->src_rect.y + para->src_rect.h)
			> para->src_image.h) {
		para->src_rect.h = para->src_image.h - para->src_rect.y;
	}

	if (((para->dst_rect.x < 0) &&
				((-para->dst_rect.x) < para->dst_rect.w))) {
		para->dst_rect.w = para->dst_rect.w + para->dst_rect.x;
		para->dst_rect.x = 0;
	} else if ((para->dst_rect.x + para->dst_rect.w)
			> para->dst_image.w) {
		para->dst_rect.w = para->dst_image.w - para->dst_rect.x;
	}
	if (((para->dst_rect.y < 0) &&
				((-para->dst_rect.y) < para->dst_rect.h))) {
		para->dst_rect.h = para->dst_rect.h + para->dst_rect.y;
		para->dst_rect.y = 0;
	} else if ((para->dst_rect.y + para->dst_rect.h)
			> para->dst_image.h) {
		para->dst_rect.h = para->dst_image.h - para->dst_rect.y;
	}

	g2d_ext_hd.finish_flag = 0;

	/* Add support inverted order copy, however,
	 * hardware have a bug when reciving y coordinate,
	 * it use (y + height) rather than (y) on inverted
	 * order mode, so here adjust it before pass it to hardware.
	 */

	mutex_lock(&global_lock);
	if (scan_order > G2D_SM_TDRL)
		para->dst_rect.y += para->src_rect.h;
	mutex_unlock(&global_lock);

	err = mixer_stretchblt(para, scan_order);

	return err;
}

#ifdef G2D_V2X_SUPPORT
int g2d_fill_h(g2d_fillrect_h *para)
{
	__s32 ret = 0;
#ifdef USE_DMA_BUF
	struct dmabuf_item *dst_item = NULL;

	dst_item = kmalloc(sizeof(struct dmabuf_item),
			      GFP_KERNEL | __GFP_ZERO);
	if (dst_item == NULL) {
		pr_err("malloc memory of size %u fail!\n",
		       sizeof(struct dmabuf_item));
		goto EXIT;
	}
#endif
	/* check the parameter valid */
	if (((para->dst_image_h.clip_rect.x < 0) &&
	     ((-para->dst_image_h.clip_rect.x) >
	      para->dst_image_h.clip_rect.w)) ||
	    ((para->dst_image_h.clip_rect.y < 0) &&
	     ((-para->dst_image_h.clip_rect.y) >
	      para->dst_image_h.clip_rect.h)) ||
	    ((para->dst_image_h.clip_rect.x > 0) &&
	     (para->dst_image_h.clip_rect.x > para->dst_image_h.width - 1))
	    || ((para->dst_image_h.clip_rect.y > 0) &&
		(para->dst_image_h.clip_rect.y >
		 para->dst_image_h.height - 1))) {
		pr_err("invalid fillrect parameter setting\n");
		return -EINVAL;
	}
	if (((para->dst_image_h.clip_rect.x < 0) &&
				((-para->dst_image_h.clip_rect.x) <
				 para->dst_image_h.clip_rect.w))) {
		para->dst_image_h.clip_rect.w =
			para->dst_image_h.clip_rect.w +
			para->dst_image_h.clip_rect.x;
		para->dst_image_h.clip_rect.x = 0;
	} else if ((para->dst_image_h.clip_rect.x +
				para->dst_image_h.clip_rect.w)
			> para->dst_image_h.width) {
		para->dst_image_h.clip_rect.w =
			para->dst_image_h.width -
			para->dst_image_h.clip_rect.x;
	}
	if (((para->dst_image_h.clip_rect.y < 0) &&
				((-para->dst_image_h.clip_rect.y) <
				 para->dst_image_h.clip_rect.h))) {
		para->dst_image_h.clip_rect.h =
			para->dst_image_h.clip_rect.h +
			para->dst_image_h.clip_rect.y;
		para->dst_image_h.clip_rect.y = 0;
	} else if ((para->dst_image_h.clip_rect.y +
				para->dst_image_h.clip_rect.h)
			> para->dst_image_h.height) {
		para->dst_image_h.clip_rect.h =
			para->dst_image_h.height -
			para->dst_image_h.clip_rect.y;
	}

	para->dst_image_h.bbuff = 1;
	para->dst_image_h.gamut = G2D_BT709;
	para->dst_image_h.mode = 0;

	g2d_ext_hd.finish_flag = 0;

#ifdef USE_DMA_BUF
	ret = g2d_dma_map(para->dst_image_h.fd, dst_item);
	if (ret != 0) {
		pr_err("map cur_item fail!\n");
		goto FREE_DST;
	}

	g2d_set_info(&para->dst_image_h, dst_item);
#endif

	ret = g2d_fillrectangle(&para->dst_image_h, para->dst_image_h.color);

	if (ret)
		pr_warn("G2D FILLRECTANGLE Failed!\n");
#ifdef USE_DMA_BUF
	g2d_dma_unmap(dst_item);
FREE_DST:
	kfree(dst_item);
EXIT:
	return ret;
#else
	return ret;
#endif
}

int g2d_blit_h(g2d_blt_h *para)
{
	__s32 ret = 0;

#ifdef USE_DMA_BUF
	struct dmabuf_item *src_item = NULL;
	struct dmabuf_item *dst_item = NULL;

	src_item = kmalloc(sizeof(struct dmabuf_item),
			      GFP_KERNEL | __GFP_ZERO);
	if (src_item == NULL) {
		pr_err("malloc memory of size %u fail!\n",
		       sizeof(struct dmabuf_item));
		goto EXIT;
	}
	dst_item = kmalloc(sizeof(struct dmabuf_item),
			      GFP_KERNEL | __GFP_ZERO);
	if (dst_item == NULL) {
		pr_err("malloc memory of size %u fail!\n",
		       sizeof(struct dmabuf_item));
		goto FREE_SRC;
	}

#endif
	/* check the parameter valid */
	if (((para->src_image_h.clip_rect.x < 0) &&
	     ((-para->src_image_h.clip_rect.x) >
	      para->src_image_h.clip_rect.w)) ||
	    ((para->src_image_h.clip_rect.y < 0) &&
	     ((-para->src_image_h.clip_rect.y) >
	      para->src_image_h.clip_rect.h)) ||
	    ((para->src_image_h.clip_rect.x > 0) &&
	     (para->src_image_h.clip_rect.x >
	      para->src_image_h.width - 1)) ||
	    ((para->src_image_h.clip_rect.y > 0) &&
	     (para->src_image_h.clip_rect.y >
	      para->src_image_h.height - 1)) ||
	    ((para->dst_image_h.clip_rect.x > 0) &&
	     (para->dst_image_h.clip_rect.x >
	      para->dst_image_h.width - 1)) ||
	    ((para->dst_image_h.clip_rect.y > 0) &&
	     (para->dst_image_h.clip_rect.y > para->dst_image_h.height - 1))) {
		pr_err("invalid bitblit parameter setting\n");
		return -EINVAL;
	}
	if (((para->src_image_h.clip_rect.x < 0) &&
				((-para->src_image_h.clip_rect.x) <
				 para->src_image_h.clip_rect.w))) {
		para->src_image_h.clip_rect.w =
			para->src_image_h.clip_rect.w +
			para->src_image_h.clip_rect.x;
		para->src_image_h.clip_rect.x = 0;
	} else if ((para->src_image_h.clip_rect.x +
				para->src_image_h.clip_rect.w)
			> para->src_image_h.width) {
		para->src_image_h.clip_rect.w =
			para->src_image_h.width -
			para->src_image_h.clip_rect.x;
	}
	if (((para->src_image_h.clip_rect.y < 0) &&
				((-para->src_image_h.clip_rect.y) <
				 para->src_image_h.clip_rect.h))) {
		para->src_image_h.clip_rect.h =
			para->src_image_h.clip_rect.h +
			para->src_image_h.clip_rect.y;
		para->src_image_h.clip_rect.y = 0;
	} else if ((para->src_image_h.clip_rect.y +
				para->src_image_h.clip_rect.h)
			> para->src_image_h.height) {
		para->src_image_h.clip_rect.h =
			para->src_image_h.height -
			para->src_image_h.clip_rect.y;
	}

	if (((para->dst_image_h.clip_rect.x < 0) &&
				((-para->dst_image_h.clip_rect.x) <
				 para->dst_image_h.clip_rect.w))) {
		para->dst_image_h.clip_rect.w =
			para->dst_image_h.clip_rect.w +
			para->dst_image_h.clip_rect.x;
		para->dst_image_h.clip_rect.x = 0;
	} else if ((para->dst_image_h.clip_rect.x +
				para->dst_image_h.clip_rect.w)
			> para->dst_image_h.width) {
		para->dst_image_h.clip_rect.w =
			para->dst_image_h.width -
			para->dst_image_h.clip_rect.x;
	}
	if (((para->dst_image_h.clip_rect.y < 0) &&
				((-para->dst_image_h.clip_rect.y) <
				 para->dst_image_h.clip_rect.h))) {
		para->dst_image_h.clip_rect.h =
			para->dst_image_h.clip_rect.h +
			para->dst_image_h.clip_rect.y;
		para->dst_image_h.clip_rect.y = 0;
	} else if ((para->dst_image_h.clip_rect.y +
				para->dst_image_h.clip_rect.h)
			> para->dst_image_h.height) {
		para->dst_image_h.clip_rect.h =
			para->dst_image_h.height -
			para->dst_image_h.clip_rect.y;
	}

	g2d_ext_hd.finish_flag = 0;

	/* Add support inverted order copy, however,
	 * hardware have a bug when reciving y coordinate,
	 * it use (y + height) rather than (y) on inverted
	 * order mode, so here adjust it before pass it to hardware.
	 */

	para->src_image_h.bpremul = 0;
	para->src_image_h.bbuff = 1;
	para->src_image_h.gamut = G2D_BT709;

	para->dst_image_h.bpremul = 0;
	para->dst_image_h.bbuff = 1;
	para->dst_image_h.gamut = G2D_BT709;

#ifdef USE_DMA_BUF
	ret = g2d_dma_map(para->src_image_h.fd, src_item);
	if (ret != 0) {
		pr_err("map cur_item fail!\n");
		goto FREE_DST;
	}
	ret = g2d_dma_map(para->dst_image_h.fd, dst_item);
	if (ret != 0) {
		pr_err("map pre_item fail!\n");
		goto SRC_DMA_UNMAP;
	}

	g2d_set_info(&para->src_image_h, src_item);
	g2d_set_info(&para->dst_image_h, dst_item);
#endif
	ret = g2d_bsp_bitblt(&para->src_image_h,
					&para->dst_image_h, para->flag_h);

	if (ret)
		pr_warn("G2D BITBLT Failed!\n");

#ifdef USE_DMA_BUF
	g2d_dma_unmap(dst_item);
SRC_DMA_UNMAP:
	g2d_dma_unmap(src_item);
FREE_DST:
	kfree(dst_item);
FREE_SRC:
	kfree(src_item);
EXIT:
	return ret;
#else
	return ret;
#endif
}

int g2d_bld_h(g2d_bld *para)
{
	__s32 ret = 0;

#ifdef USE_DMA_BUF
	struct dmabuf_item *src_item = NULL;
	struct dmabuf_item *dst_item = NULL;

	src_item = kmalloc(sizeof(struct dmabuf_item),
			      GFP_KERNEL | __GFP_ZERO);
	if (src_item == NULL) {
		pr_err("malloc memory of size %u fail!\n",
		       sizeof(struct dmabuf_item));
		goto EXIT;
	}
	dst_item = kmalloc(sizeof(struct dmabuf_item),
			      GFP_KERNEL | __GFP_ZERO);
	if (dst_item == NULL) {
		pr_err("malloc memory of size %u fail!\n",
		       sizeof(struct dmabuf_item));
		goto FREE_SRC;
	}

#endif
	/* check the parameter valid */
	if (((para->src_image_h.clip_rect.x < 0) &&
	     ((-para->src_image_h.clip_rect.x) >
	      para->src_image_h.clip_rect.w)) ||
	    ((para->src_image_h.clip_rect.y < 0) &&
	     ((-para->src_image_h.clip_rect.y) >
	      para->src_image_h.clip_rect.h)) ||
	    ((para->src_image_h.clip_rect.x > 0) &&
	     (para->src_image_h.clip_rect.x >
	      para->src_image_h.width - 1)) ||
	    ((para->src_image_h.clip_rect.y > 0) &&
	     (para->src_image_h.clip_rect.y >
	      para->src_image_h.height - 1)) ||
	    ((para->dst_image_h.clip_rect.x > 0) &&
	     (para->dst_image_h.clip_rect.x > para->dst_image_h.width - 1))
	    || ((para->dst_image_h.clip_rect.y > 0) &&
		(para->dst_image_h.clip_rect.y >
		 para->dst_image_h.height - 1))) {
		pr_err("invalid blit parameter setting\n");
		return -EINVAL;
	}
	if (((para->src_image_h.clip_rect.x < 0) &&
				((-para->src_image_h.clip_rect.x) <
				 para->src_image_h.clip_rect.w))) {
		para->src_image_h.clip_rect.w =
			para->src_image_h.clip_rect.w +
			para->src_image_h.clip_rect.x;
		para->src_image_h.clip_rect.x = 0;
	} else if ((para->src_image_h.clip_rect.x +
				para->src_image_h.clip_rect.w)
			> para->src_image_h.width) {
		para->src_image_h.clip_rect.w =
			para->src_image_h.width -
			para->src_image_h.clip_rect.x;
	}
	if (((para->src_image_h.clip_rect.y < 0) &&
				((-para->src_image_h.clip_rect.y) <
				 para->src_image_h.clip_rect.h))) {
		para->src_image_h.clip_rect.h =
			para->src_image_h.clip_rect.h +
			para->src_image_h.clip_rect.y;
		para->src_image_h.clip_rect.y = 0;
	} else if ((para->src_image_h.clip_rect.y +
				para->src_image_h.clip_rect.h)
			> para->src_image_h.height) {
		para->src_image_h.clip_rect.h =
			para->src_image_h.height -
			para->src_image_h.clip_rect.y;
	}

	para->src_image_h.bpremul = 0;
	para->src_image_h.bbuff = 1;
	para->src_image_h.gamut = G2D_BT709;

	para->dst_image_h.bpremul = 0;
	para->dst_image_h.bbuff = 1;
	para->dst_image_h.gamut = G2D_BT709;

	g2d_ext_hd.finish_flag = 0;

#ifdef USE_DMA_BUF
	ret = g2d_dma_map(para->src_image_h.fd, src_item);
	if (ret != 0) {
		pr_err("map cur_item fail!\n");
		goto FREE_DST;
	}
	ret = g2d_dma_map(para->dst_image_h.fd, dst_item);
	if (ret != 0) {
		pr_err("map pre_item fail!\n");
		goto SRC_DMA_UNMAP;
	}

	g2d_set_info(&para->src_image_h, src_item);
	g2d_set_info(&para->dst_image_h, dst_item);
#endif
	ret = g2d_bsp_bld(&para->src_image_h, &para->dst_image_h,
						para->bld_cmd, &para->ck_para);

	if (ret)
		pr_warn("G2D BITBLT Failed!\n");

#ifdef USE_DMA_BUF
	g2d_dma_unmap(dst_item);
SRC_DMA_UNMAP:
	g2d_dma_unmap(src_item);
FREE_DST:
	kfree(dst_item);
FREE_SRC:
	kfree(src_item);
EXIT:
	return ret;
#else
	return ret;
#endif
}

int g2d_maskblt_h(g2d_maskblt *para)
{
	__s32 ret = 0;

#ifdef USE_DMA_BUF
	struct dmabuf_item *src_item = NULL;
	struct dmabuf_item *ptn_item = NULL;
	struct dmabuf_item *mask_item = NULL;
	struct dmabuf_item *dst_item = NULL;

	src_item = kmalloc(sizeof(struct dmabuf_item),
			      GFP_KERNEL | __GFP_ZERO);
	if (src_item == NULL) {
		pr_err("malloc memory of size %u fail!\n",
		       sizeof(struct dmabuf_item));
		goto EXIT;
	}
	ptn_item = kmalloc(sizeof(struct dmabuf_item),
			      GFP_KERNEL | __GFP_ZERO);
	if (ptn_item == NULL) {
		pr_err("malloc memory of size %u fail!\n",
		       sizeof(struct dmabuf_item));
		goto FREE_SRC;
	}

	mask_item = kmalloc(sizeof(struct dmabuf_item),
			      GFP_KERNEL | __GFP_ZERO);
	if (mask_item == NULL) {
		pr_err("malloc memory of size %u fail!\n",
		       sizeof(struct dmabuf_item));
		goto FREE_PTN;
	}
	dst_item = kmalloc(sizeof(struct dmabuf_item),
			      GFP_KERNEL | __GFP_ZERO);
	if (dst_item == NULL) {
		pr_err("malloc memory of size %u fail!\n",
		       sizeof(struct dmabuf_item));
		goto FREE_MASK;
	}

#endif
	/* check the parameter valid */
	if (((para->dst_image_h.clip_rect.x < 0) &&
	     ((-para->dst_image_h.clip_rect.x) >
	      para->dst_image_h.clip_rect.w)) ||
	    ((para->dst_image_h.clip_rect.y < 0) &&
	     ((-para->dst_image_h.clip_rect.y) >
	      para->dst_image_h.clip_rect.h)) ||
	    ((para->dst_image_h.clip_rect.x > 0) &&
	     (para->dst_image_h.clip_rect.x >
	      para->dst_image_h.width - 1)) ||
	    ((para->dst_image_h.clip_rect.y > 0) &&
	     (para->dst_image_h.clip_rect.y > para->dst_image_h.height - 1))) {
		pr_err("invalid maskblt parameter setting\n");
		return -EINVAL;
	}
	if (((para->dst_image_h.clip_rect.x < 0) &&
				((-para->dst_image_h.clip_rect.x) <
				 para->dst_image_h.clip_rect.w))) {
		para->dst_image_h.clip_rect.w =
			para->dst_image_h.clip_rect.w +
			para->dst_image_h.clip_rect.x;
		para->dst_image_h.clip_rect.x = 0;
	} else if ((para->dst_image_h.clip_rect.x +
				para->dst_image_h.clip_rect.w)
			> para->dst_image_h.width) {
		para->dst_image_h.clip_rect.w =
			para->dst_image_h.width -
			para->dst_image_h.clip_rect.x;
	}
	if (((para->dst_image_h.clip_rect.y < 0) &&
				((-para->dst_image_h.clip_rect.y) <
				 para->dst_image_h.clip_rect.h))) {
		para->dst_image_h.clip_rect.h =
			para->dst_image_h.clip_rect.h +
			para->dst_image_h.clip_rect.y;
		para->dst_image_h.clip_rect.y = 0;
	} else if ((para->dst_image_h.clip_rect.y +
				para->dst_image_h.clip_rect.h)
			> para->dst_image_h.height) {
		para->dst_image_h.clip_rect.h =
			para->dst_image_h.height -
			para->dst_image_h.clip_rect.y;
	}

#ifdef USE_DMA_BUF
	ret = g2d_dma_map(para->src_image_h.fd, src_item);
	if (ret != 0) {
		pr_err("map cur_item fail!\n");
		goto FREE_DST;
	}
	ret = g2d_dma_map(para->ptn_image_h.fd, ptn_item);
	if (ret != 0) {
		pr_err("map pre_item fail!\n");
		goto SRC_DMA_UNMAP;
	}
	ret = g2d_dma_map(para->mask_image_h.fd, mask_item);
	if (ret != 0) {
		pr_err("map cur_item fail!\n");
		goto PTN_DMA_UNMAP;
	}
	ret = g2d_dma_map(para->dst_image_h.fd, dst_item);
	if (ret != 0) {
		pr_err("map pre_item fail!\n");
		goto MASK_DMA_UNMAP;
	}

	g2d_set_info(&para->src_image_h, src_item);
	g2d_set_info(&para->ptn_image_h, ptn_item);
	g2d_set_info(&para->mask_image_h, mask_item);
	g2d_set_info(&para->dst_image_h, dst_item);
#endif

	para->src_image_h.bbuff = 1;
	para->src_image_h.gamut = G2D_BT709;

	para->ptn_image_h.bbuff = 1;
	para->ptn_image_h.gamut = G2D_BT709;

	para->mask_image_h.bbuff = 1;
	para->mask_image_h.gamut = G2D_BT709;

	para->dst_image_h.bbuff = 1;
	para->dst_image_h.gamut = G2D_BT709;

	g2d_ext_hd.finish_flag = 0;

	ret =
	    g2d_bsp_maskblt(&para->src_image_h, &para->ptn_image_h,
			    &para->mask_image_h, &para->dst_image_h,
			    para->back_flag, para->fore_flag);

	if (ret)
		pr_warn("G2D MASKBLT Failed!\n");
#ifdef USE_DMA_BUF
	g2d_dma_unmap(dst_item);
MASK_DMA_UNMAP:
	g2d_dma_unmap(mask_item);
PTN_DMA_UNMAP:
	g2d_dma_unmap(ptn_item);
SRC_DMA_UNMAP:
	g2d_dma_unmap(src_item);
FREE_DST:
	kfree(dst_item);
FREE_MASK:
	kfree(mask_item);
FREE_PTN:
	kfree(ptn_item);
FREE_SRC:
	kfree(src_item);
EXIT:
	return ret;
#else
	return ret;
#endif
}
#endif

/*
int g2d_set_palette_table(g2d_palette *para)
{

	if ((para->pbuffer == NULL) || (para->size < 0) ||
			(para->size > 1024)) {
		pr_warn("para invalid in mixer_set_palette\n");
		return -1;
	}

	mixer_set_palette(para);

	return 0;
}
*/

/*
int g2d_cmdq(unsigned int para)
{
	__s32 err = 0;

	g2d_ext_hd.finish_flag = 0;
	err = mixer_cmdq(para);

	return err;
}
*/

long g2d_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	__s32 ret = 0;

	if (!mutex_trylock(&para.mutex))
		mutex_lock(&para.mutex);
	switch (cmd) {

		/* Proceed to the operation */
	case G2D_CMD_BITBLT:{
			g2d_blt blit_para;

			if (copy_from_user(&blit_para, (g2d_blt *) arg,
					   sizeof(g2d_blt))) {
				kfree(&blit_para);
				ret = -EFAULT;
				goto err_noput;
			}
			ret = g2d_blit(&blit_para);
			break;
		}
	case G2D_CMD_FILLRECT:{
			g2d_fillrect fill_para;

			if (copy_from_user(&fill_para, (g2d_fillrect *) arg,
					   sizeof(g2d_fillrect))) {
				kfree(&fill_para);
				ret = -EFAULT;
				goto err_noput;
			}
			ret = g2d_fill(&fill_para);
			break;
		}
	case G2D_CMD_STRETCHBLT:{
			g2d_stretchblt stre_para;

			if (copy_from_user(&stre_para, (g2d_stretchblt *) arg,
					   sizeof(g2d_stretchblt))) {
				kfree(&stre_para);
				ret = -EFAULT;
				goto err_noput;
			}
			ret = g2d_stretchblit(&stre_para);
			break;
		}
/*	case G2D_CMD_PALETTE_TBL:{
		g2d_palette pale_para;

		if (copy_from_user(&pale_para, (g2d_palette *)arg,
					sizeof(g2d_palette))) {
			kfree(&pale_para);
			ret = -EFAULT;
			goto err_noput;
		}
		ret = g2d_set_palette_table(&pale_para);
		break;
	}
	case G2D_CMD_QUEUE:{
		unsigned int cmdq_addr;

		if (copy_from_user(&cmdq_addr,
				(unsigned int *)arg, sizeof(unsigned int))) {
			kfree(&cmdq_addr);
			ret = -EFAULT;
			goto err_noput;
		}
		ret = g2d_cmdq(cmdq_addr);
		break;
	}
*/
#ifdef G2D_V2X_SUPPORT
	case G2D_CMD_BITBLT_H:{
			g2d_blt_h blit_para;

			if (copy_from_user(&blit_para, (g2d_blt_h *) arg,
					   sizeof(g2d_blt_h))) {
				kfree(&blit_para);
				ret = -EFAULT;
				goto err_noput;
			}
			ret = g2d_blit_h(&blit_para);
			break;
		}
	case G2D_CMD_FILLRECT_H:{
			g2d_fillrect_h fill_para;

			if (copy_from_user(&fill_para, (g2d_fillrect_h *) arg,
					   sizeof(g2d_fillrect_h))) {
				kfree(&fill_para);
				ret = -EFAULT;
				goto err_noput;
			}
			ret = g2d_fill_h(&fill_para);
			break;
		}
	case G2D_CMD_BLD_H:{
			g2d_bld bld_para;

			if (copy_from_user(&bld_para, (g2d_bld *) arg,
					   sizeof(g2d_bld))) {
				kfree(&bld_para);
				ret = -EFAULT;
				goto err_noput;
			}
			ret = g2d_bld_h(&bld_para);
			break;
		}
	case G2D_CMD_MASK_H:{
			g2d_maskblt mask_para;

			if (copy_from_user(&mask_para, (g2d_maskblt *) arg,
					   sizeof(g2d_maskblt))) {
				kfree(&mask_para);
				ret = -EFAULT;
				goto err_noput;
			}
			ret = g2d_maskblt_h(&mask_para);
			break;
		}
#endif
		/* just management memory for test */
	case G2D_CMD_MEM_REQUEST:
		ret = g2d_mem_request(arg);
		break;

	case G2D_CMD_MEM_RELEASE:
		ret = g2d_mem_release(arg);
		break;

	case G2D_CMD_MEM_SELIDX:
		g2d_mem_sel = arg;
		break;

	case G2D_CMD_MEM_GETADR:
		if (g2d_mem[arg].b_used) {
			ret = g2d_mem[arg].phy_addr;
		} else {
			ERR("mem not used in G2D_CMD_MEM_GETADR\n");
			ret = -1;
		}
		break;

	case G2D_CMD_INVERTED_ORDER:
		{
			if (arg > G2D_SM_DTRL) {
				ERR("scan mode is err.\n");
				ret = -EINVAL;
				goto err_noput;
			}

			mutex_lock(&global_lock);
			scan_order = arg;
			mutex_unlock(&global_lock);
			break;
		}

		/* Invalid IOCTL call */
	default:
		return -EINVAL;
	}

err_noput:
	mutex_unlock(&para.mutex);

	return ret;
}

static const struct file_operations g2d_fops = {
	.owner = THIS_MODULE,
	.open = g2d_open,
	.release = g2d_release,
	.unlocked_ioctl = g2d_ioctl,
	.mmap = g2d_mmap,
};

static u64 sunxi_g2d_dma_mask = DMA_BIT_MASK(32);
static int g2d_probe(struct platform_device *pdev)
{
#if !defined(CONFIG_OF)
	int size;
	struct resource *res;
#endif
	int ret = 0;
	__g2d_info_t *info = NULL;

	info = &para;
	info->dev = &pdev->dev;
	dmabuf_dev = &pdev->dev;
	dmabuf_dev->dma_mask = &sunxi_g2d_dma_mask;
	dmabuf_dev->coherent_dma_mask = DMA_BIT_MASK(32);
	platform_set_drvdata(pdev, info);

#if !defined(CONFIG_OF)
	/* get the memory region */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (res == NULL) {
		ERR("failed to get memory register\n");
		ret = -ENXIO;
		goto dealloc_fb;
	}

	size = (res->end - res->start) + 1;
	/* map the memory */
	info->io = ioremap(res->start, size);
	if (info->io == NULL) {
		ERR("iorGmap() of register failed\n");
		ret = -ENXIO;
		goto dealloc_fb;
	}
#else
	info->io = of_iomap(pdev->dev.of_node, 0);
	if (info->io == NULL) {
		ERR("iormap() of register failed\n");
		ret = -ENXIO;
		goto dealloc_fb;
	}
#endif

#if !defined(CONFIG_OF)
	/* get the irq */
	res = platform_get_resource(pdev, IORESOURCE_IRQ, 0);
	if (res == NULL) {
		ERR("failed to get irq resource\n");
		ret = -ENXIO;
		goto release_regs;
	}
	info->irq = res->start;
#else
	info->irq = irq_of_parse_and_map(pdev->dev.of_node, 0);
	if (!info->irq) {
		ERR("irq_of_parse_and_map irq fail for transform\n");
		ret = -ENXIO;
		goto release_regs;
	}
#endif

	/* request the irq */
	ret = request_irq(info->irq, g2d_handle_irq, 0,
			  dev_name(&pdev->dev), NULL);
	if (ret) {
		ERR("failed to install irq resource\n");
		goto release_regs;
	}
#if defined(CONFIG_OF)
	/* clk init */
	info->clk = of_clk_get(pdev->dev.of_node, 0);
	if (IS_ERR(info->clk))
		ERR("fail to get clk\n");
#endif

	drv_g2d_init();
	mutex_init(&info->mutex);
	mutex_init(&global_lock);

	ret = sysfs_create_group(&g2d_dev->kobj, &g2d_attribute_group);
	if (ret < 0)
		WARNING("sysfs_create_file fail!\n");

	return 0;

release_regs:
#if !defined(CONFIG_OF)
	iounmap(info->io);
#endif
dealloc_fb:
	platform_set_drvdata(pdev, NULL);

	return ret;
}

static int g2d_remove(struct platform_device *pdev)
{
	__g2d_info_t *info = platform_get_drvdata(pdev);

	free_irq(info->irq, NULL);
#if !defined(CONFIG_OF)
	iounmap(info->io);
#endif
	platform_set_drvdata(pdev, NULL);

	sysfs_remove_group(&g2d_dev->kobj, &g2d_attribute_group);

	INFO("Driver unloaded succesfully.\n");
	return 0;
}

static int g2d_suspend(struct platform_device *pdev, pm_message_t state)
{
	INFO("%s.\n", __func__);
	mutex_lock(&para.mutex);
	if (para.opened) {
		if (para.clk)
			clk_disable(para.clk);
	}
	mutex_unlock(&para.mutex);
	INFO("g2d_suspend succesfully.\n");

	return 0;
}

static int g2d_resume(struct platform_device *pdev)
{
	INFO("%s.\n", __func__);
	mutex_lock(&para.mutex);
	if (para.opened) {
		if (para.clk)
			clk_prepare_enable(para.clk);
	}
	mutex_unlock(&para.mutex);
	INFO("g2d_resume succesfully.\n");

	return 0;
}

#if !defined(CONFIG_OF)
struct platform_device g2d_device = {

	.name = "g2d",
	.id = -1,
	.num_resources = ARRAY_SIZE(g2d_resource),
	.resource = g2d_resource,
	.dev = {

		},
};
#else
static const struct of_device_id sunxi_g2d_match[] = {
	{.compatible = "allwinner,sunxi-g2d",},
	{},
};
#endif

static struct platform_driver g2d_driver = {
	.probe = g2d_probe,
	.remove = g2d_remove,
	.suspend = g2d_suspend,
	.resume = g2d_resume,
	.suspend = NULL,
	.resume = NULL,
	.driver = {

		   .owner = THIS_MODULE,
		   .name = "g2d",
		   .of_match_table = sunxi_g2d_match,
		   },
};

int __init g2d_module_init(void)
{
	int ret = 0, err;

	alloc_chrdev_region(&devid, 0, 1, "g2d_chrdev");
	g2d_cdev = cdev_alloc();
	cdev_init(g2d_cdev, &g2d_fops);
	g2d_cdev->owner = THIS_MODULE;
	err = cdev_add(g2d_cdev, devid, 1);
	if (err) {
		ERR("I was assigned major number %d.\n", MAJOR(devid));
		return -1;
	}

	g2d_class = class_create(THIS_MODULE, "g2d");
	if (IS_ERR(g2d_class)) {
		ERR("create class error\n");
		return -1;
	}

	g2d_dev = device_create(g2d_class, NULL, devid, NULL, "g2d");
#if !defined(CONFIG_OF)
	ret = platform_device_register(&g2d_device);
#endif
	if (ret == 0)
		ret = platform_driver_register(&g2d_driver);

	INFO("Module initialized.major:%d\n", MAJOR(devid));
	return ret;
}

static void __exit g2d_module_exit(void)
{
	INFO("g2d_module_exit\n");
	kfree(g2d_ext_hd.g2d_finished_sem);

	platform_driver_unregister(&g2d_driver);
#if !defined(CONFIG_OF)
	platform_device_unregister(&g2d_device);
#endif
	device_destroy(g2d_class, devid);
	class_destroy(g2d_class);

	cdev_del(g2d_cdev);
}

module_init(g2d_module_init);
module_exit(g2d_module_exit);

MODULE_AUTHOR("yupu_tang");
MODULE_AUTHOR("tyle <tyle@allwinnertech.com>");
MODULE_DESCRIPTION("g2d driver");
MODULE_LICENSE("GPL");
