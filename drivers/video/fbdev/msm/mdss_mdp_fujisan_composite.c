/*
 * Fujisan (ZTE Axon M) Composite C stage-1 test backend.
 *
 * The two DSI panels are normally registered as independent fbdev devices.
 * This debugfs-gated test never takes Android scanout ownership at boot and
 * accepts no userspace dma-buf yet.  It verifies a fixed linear buffer's
 * crops on the native CTLs without retrofitting a live CTL pair into
 * MDP_DUAL_LM_DUAL_DISPLAY.  That topology must instead exist before the
 * command contexts and their resource work are started.
 */

#define pr_fmt(fmt) "fujisan-composite: " fmt

#include <linux/delay.h>
#include <linux/dma-buf.h>
#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/fb.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "mdss_fb.h"
#include "mdss_mdp.h"
#include "mdss_mdp_trace.h"
#include "mdss_smmu.h"

#define FUJISAN_WIDE_WIDTH	2160
#define FUJISAN_WIDE_HEIGHT	1915
#define FUJISAN_SPLIT_X	1080
#define FUJISAN_B_PAD_TOP	5
#define FUJISAN_TEST_BPP	4
#define FUJISAN_TEST_SIZE	(FUJISAN_WIDE_WIDTH * FUJISAN_WIDE_HEIGHT * \
				 FUJISAN_TEST_BPP)

struct fujisan_composite_buffer {
	struct dma_buf *dma_buf;
	struct dma_buf_attachment *attachment;
	struct sg_table *table;
	dma_addr_t iova;
	unsigned long size;
};

struct fujisan_composite_link {
	struct msm_fb_data_type *left_mfd;
	struct msm_fb_data_type *right_mfd;
	struct mdss_mdp_ctl *left_ctl;
	struct mdss_mdp_ctl *right_ctl;
	bool native_topology;
	bool linked;
};

struct fujisan_composite_state {
	struct mutex lock;
	struct fujisan_composite_link link;
	struct fujisan_composite_buffer buffer;
	struct mdss_mdp_pipe *left_pipe;
	struct mdss_mdp_pipe *right_pipe;
	struct mdss_mdp_data *left_data;
	struct mdss_mdp_data *right_data;
	bool left_op_enable;
	bool right_op_enable;
	bool iommu_attached;
	bool active;
	u64 submit_seq;
	int last_error;
	const char *last_stage;
	struct dentry *debug_root;
};

static struct fujisan_composite_state fujisan_composite = {
	.lock = __MUTEX_INITIALIZER(fujisan_composite.lock),
	.last_stage = "idle",
};

static void fujisan_trace(const char *event, int rc)
{
	struct fujisan_composite_link *link = &fujisan_composite.link;
	u32 ctl = link->left_ctl ? link->left_ctl->num : 0;
	bool video = link->left_ctl ? link->left_ctl->is_video_mode : false;

	trace_fujisan_display_event(ctl, 2, video, event, rc);
}

static void fujisan_trace_ctl(const char *event, struct mdss_mdp_ctl *ctl,
	int rc)
{
	trace_fujisan_display_event(ctl ? ctl->num : 0, 2,
		ctl ? ctl->is_video_mode : false, event, rc);
}

static int fujisan_get_mfds(struct msm_fb_data_type **left,
	struct msm_fb_data_type **right)
{
	struct fb_info *left_info = registered_fb[0];
	struct fb_info *right_info = registered_fb[1];

	if (!left_info || !right_info || !left_info->par || !right_info->par)
		return -ENODEV;

	*left = left_info->par;
	*right = right_info->par;
	if (!(*left)->mdp.private1 || !(*right)->mdp.private1)
		return -ENODEV;

	return 0;
}

/*
 * The stage-1 DTS switch registers both physical panels on fb0.  fb0/ctl0
 * is physical B (the logical right half) and its native split CTL is physical
 * A (the logical left half).  This must be present before Android starts any
 * command context; do not turn two already-live fbdev CTLs into this topology.
 */
static int fujisan_get_native_mfd(struct msm_fb_data_type **mfd_out)
{
	struct fb_info *info = registered_fb[0];
	struct msm_fb_data_type *mfd;
	struct mdss_panel_data *pdata;

	if (!info || !info->par)
		return -ENODEV;

	mfd = info->par;
	pdata = dev_get_platdata(&mfd->pdev->dev);
	if (!mfd->mdp.private1 || !pdata || !pdata->next ||
	    mfd->split_mode != MDP_DUAL_LM_DUAL_DISPLAY)
		return -EOPNOTSUPP;
	*mfd_out = mfd;
	return 0;
}

static int fujisan_link_native_ctls(struct msm_fb_data_type *mfd)
{
	struct mdss_panel_data *pdata = dev_get_platdata(&mfd->pdev->dev);
	struct mdss_mdp_ctl *ctl = mfd_to_ctl(mfd);
	struct mdss_mdp_ctl *sctl;

	if (!pdata || !pdata->next || !ctl || !ctl->is_master)
		return -EOPNOTSUPP;

	sctl = mdss_mdp_get_split_ctl(ctl);
	if (!sctl || sctl->mfd != mfd || sctl->panel_data != pdata->next)
		return -EOPNOTSUPP;

	fujisan_composite.link.left_mfd = mfd;
	fujisan_composite.link.right_mfd = mfd;
	fujisan_composite.link.left_ctl = ctl;
	fujisan_composite.link.right_ctl = sctl;
	fujisan_composite.link.native_topology = true;
	fujisan_composite.link.linked = true;

	pr_info("native topology: fb%d/ctl%d=B (right), ctl%d=A (left)\n",
		mfd->index, ctl->num, sctl->num);
	return 0;
}

static void fujisan_lock_ov(struct msm_fb_data_type *left,
	struct msm_fb_data_type *right)
{
	struct mdss_overlay_private *left_mdp = mfd_to_mdp5_data(left);
	struct mdss_overlay_private *right_mdp = mfd_to_mdp5_data(right);

	mutex_lock(&left_mdp->ov_lock);
	mutex_lock(&right_mdp->ov_lock);
}

static void fujisan_unlock_ov(struct msm_fb_data_type *left,
	struct msm_fb_data_type *right)
{
	struct mdss_overlay_private *left_mdp = mfd_to_mdp5_data(left);
	struct mdss_overlay_private *right_mdp = mfd_to_mdp5_data(right);

	mutex_unlock(&right_mdp->ov_lock);
	mutex_unlock(&left_mdp->ov_lock);
}

static void fujisan_lock_ov_one(struct msm_fb_data_type *mfd)
{
	struct mdss_overlay_private *mdp = mfd_to_mdp5_data(mfd);

	mutex_lock(&mdp->ov_lock);
}

static void fujisan_unlock_ov_one(struct msm_fb_data_type *mfd)
{
	struct mdss_overlay_private *mdp = mfd_to_mdp5_data(mfd);

	mutex_unlock(&mdp->ov_lock);
}

static int fujisan_claim_scanout(struct msm_fb_data_type *left,
	struct msm_fb_data_type *right)
{
	long left_ioctl_idle;
	long right_ioctl_idle;
	long left_commit_idle;
	long right_commit_idle;

	if (!left->op_enable || !right->op_enable)
		return -EPERM;
	/* fb0/ctl0 drives physical B; fb1/ctl1 drives physical A.  fb1 is brought
	 * up after legacy scanout has been quiesced through mdss_fb_panel_unblank(). */
	if (!mdss_fb_is_power_on(left))
		return -EHOSTDOWN;

	mutex_lock(&left->mdss_sysfs_lock);
	mutex_lock(&right->mdss_sysfs_lock);
	fujisan_composite.left_op_enable = left->op_enable;
	fujisan_composite.right_op_enable = right->op_enable;
	left->op_enable = false;
	right->op_enable = false;
	mutex_unlock(&right->mdss_sysfs_lock);
	mutex_unlock(&left->mdss_sysfs_lock);

	/* Let all legacy work leave both the ioctl and display-thread paths.
	 * An MDP commit temporarily drops ov_lock while retaining ctl->lock; taking
	 * ov_lock in that window would otherwise invert the native lock order. */
	left_ioctl_idle = wait_event_timeout(left->ioctl_q,
		atomic_read(&left->ioctl_ref_cnt) == 0, msecs_to_jiffies(1000));
	right_ioctl_idle = wait_event_timeout(right->ioctl_q,
		atomic_read(&right->ioctl_ref_cnt) == 0, msecs_to_jiffies(1000));
	left_commit_idle = wait_event_timeout(left->idle_wait_q,
		atomic_read(&left->commits_pending) == 0,
		msecs_to_jiffies(1000));
	right_commit_idle = wait_event_timeout(right->idle_wait_q,
		atomic_read(&right->commits_pending) == 0,
		msecs_to_jiffies(1000));
	if (!left_ioctl_idle || !right_ioctl_idle ||
	    !left_commit_idle || !right_commit_idle) {
		mutex_lock(&left->mdss_sysfs_lock);
		mutex_lock(&right->mdss_sysfs_lock);
		left->op_enable = fujisan_composite.left_op_enable;
		right->op_enable = fujisan_composite.right_op_enable;
		mutex_unlock(&right->mdss_sysfs_lock);
		mutex_unlock(&left->mdss_sysfs_lock);
		return -EBUSY;
	}

	return 0;
}

static void fujisan_release_scanout(struct msm_fb_data_type *left,
	struct msm_fb_data_type *right)
{
	mutex_lock(&left->mdss_sysfs_lock);
	mutex_lock(&right->mdss_sysfs_lock);
	left->op_enable = fujisan_composite.left_op_enable;
	right->op_enable = fujisan_composite.right_op_enable;
	mutex_unlock(&right->mdss_sysfs_lock);
	mutex_unlock(&left->mdss_sysfs_lock);
}

static int fujisan_claim_native_scanout(struct msm_fb_data_type *mfd)
{
	long ioctl_idle;
	long commit_idle;

	if (!mfd->op_enable)
		return -EPERM;
	if (!mdss_fb_is_power_on(mfd))
		return -EHOSTDOWN;

	mutex_lock(&mfd->mdss_sysfs_lock);
	fujisan_composite.left_op_enable = mfd->op_enable;
	mfd->op_enable = false;
	mutex_unlock(&mfd->mdss_sysfs_lock);

	ioctl_idle = wait_event_timeout(mfd->ioctl_q,
		atomic_read(&mfd->ioctl_ref_cnt) == 0, msecs_to_jiffies(1000));
	commit_idle = wait_event_timeout(mfd->idle_wait_q,
		atomic_read(&mfd->commits_pending) == 0, msecs_to_jiffies(1000));
	if (!ioctl_idle || !commit_idle) {
		mutex_lock(&mfd->mdss_sysfs_lock);
		mfd->op_enable = fujisan_composite.left_op_enable;
		mutex_unlock(&mfd->mdss_sysfs_lock);
		return -EBUSY;
	}

	return 0;
}

static void fujisan_release_native_scanout(struct msm_fb_data_type *mfd)
{
	mutex_lock(&mfd->mdss_sysfs_lock);
	mfd->op_enable = fujisan_composite.left_op_enable;
	mutex_unlock(&mfd->mdss_sysfs_lock);
}

static int fujisan_prepare_secondary(struct msm_fb_data_type *right)
{
	int rc;

	if (mdss_fb_is_power_on(right))
		return 0;

	/* fujisan_claim_scanout() has made fb1 unavailable to legacy ioctls. */
	mutex_lock(&right->mdss_sysfs_lock);
	rc = mdss_fb_panel_unblank(right);
	mutex_unlock(&right->mdss_sysfs_lock);
	return rc;
}

static int fujisan_link_ctls(struct msm_fb_data_type *left,
	struct msm_fb_data_type *right)
{
	struct fujisan_composite_link *link = &fujisan_composite.link;
	struct mdss_mdp_ctl *left_ctl = mfd_to_ctl(left);
	struct mdss_mdp_ctl *right_ctl = mfd_to_ctl(right);

	if (!left_ctl || !right_ctl || !left_ctl->mixer_left ||
		!right_ctl->mixer_left || !left_ctl->panel_data ||
		!right_ctl->panel_data)
		return -ENODEV;
	if (left_ctl == right_ctl)
		return -EBUSY;

	link->left_mfd = left;
	link->right_mfd = right;
	link->left_ctl = left_ctl;
	link->right_ctl = right_ctl;
	pr_info("prepared independent ctl%d/ctl%d\n", left_ctl->num,
		right_ctl->num);
	link->linked = true;

	return 0;
}

static void fujisan_unlink_ctls(void)
{
	struct fujisan_composite_link *link = &fujisan_composite.link;

	if (!link->linked)
		return;

	memset(link, 0, sizeof(*link));
}

/*
 * The MDSS dual-display contract is created before command contexts start:
 * a master MFD owns both CTLs and the slave CTL is allocated with that same
 * MFD.  Retrofitting the relation onto two live fbdev CTLs corrupts resource
 * ownership, which is why the independent diagnostic above is kept separate.
 *
 * This is intentionally a one-shot stage-1 test.  It consumes the legacy
 * CTLs after their scanout has been claimed; normal fbdev ownership is
 * restored by the host reboot following the test, not by mixing the two
 * topologies in one live command context.
 */
static void fujisan_fill_test_pattern(void *vaddr)
{
	u32 *pixels = vaddr;
	u32 x, y;

	for (y = 0; y < FUJISAN_WIDE_HEIGHT; y++) {
		for (x = 0; x < FUJISAN_WIDE_WIDTH; x++) {
			u32 color;

			if (x < FUJISAN_SPLIT_X)
				color = ((x / 135) & 1) ? 0xffff0000 : 0xffffff00;
			else
				color = (((x - FUJISAN_SPLIT_X) / 135) & 1) ?
					0xff00ffff : 0xff0000ff;

			/* A white border makes the crop edge visible on both panels. */
			if (y < 8 || y >= FUJISAN_WIDE_HEIGHT - 8 ||
			    (x % FUJISAN_SPLIT_X) < 8 ||
			    (x % FUJISAN_SPLIT_X) >= FUJISAN_SPLIT_X - 8)
				color = 0xffffffff;
			pixels[y * FUJISAN_WIDE_WIDTH + x] = color;
		}
	}
}

static int fujisan_alloc_test_buffer(struct msm_fb_data_type *mfd)
{
	struct fujisan_composite_buffer *buffer = &fujisan_composite.buffer;
	struct ion_client *client;
	struct ion_handle *handle;
	void *vaddr;
	unsigned long size = FUJISAN_TEST_SIZE;
	bool cpu_sync = false;
	int rc;

	client = mdss_get_ionclient();
	if (!client)
		return -ENODEV;

	handle = ion_alloc(client, size, SZ_4K, ION_HEAP(ION_SYSTEM_HEAP_ID), 0);
	if (IS_ERR(handle))
		return PTR_ERR(handle);
	if (!handle)
		return -ENOMEM;

	buffer->dma_buf = ion_share_dma_buf(client, handle);
	if (IS_ERR(buffer->dma_buf)) {
		rc = PTR_ERR(buffer->dma_buf);
		buffer->dma_buf = NULL;
		goto free_handle;
	}

	buffer->attachment = mdss_smmu_dma_buf_attach(buffer->dma_buf,
		&mfd->pdev->dev, MDSS_IOMMU_DOMAIN_UNSECURE);
	if (IS_ERR_OR_NULL(buffer->attachment)) {
		rc = buffer->attachment ? PTR_ERR(buffer->attachment) : -ENODEV;
		buffer->attachment = NULL;
		goto put_dma_buf;
	}

	buffer->table = dma_buf_map_attachment(buffer->attachment,
		DMA_BIDIRECTIONAL);
	if (IS_ERR_OR_NULL(buffer->table)) {
		rc = buffer->table ? PTR_ERR(buffer->table) : -ENOMEM;
		buffer->table = NULL;
		goto detach_dma_buf;
	}

	rc = mdss_smmu_map_dma_buf(buffer->dma_buf, buffer->table,
		MDSS_IOMMU_DOMAIN_UNSECURE, &buffer->iova, &size,
		DMA_BIDIRECTIONAL);
	if (rc)
		goto unmap_attachment;
	if (size < FUJISAN_TEST_SIZE) {
		rc = -EOVERFLOW;
		goto unmap_iova;
	}
	buffer->size = size;

	rc = dma_buf_begin_cpu_access(buffer->dma_buf, 0, FUJISAN_TEST_SIZE,
		DMA_BIDIRECTIONAL);
	/* Legacy ION system buffers do not implement this optional hook. */
	if (rc && rc != -EINVAL)
		goto unmap_iova;
	cpu_sync = !rc;
	vaddr = dma_buf_kmap(buffer->dma_buf, 0);
	if (IS_ERR_OR_NULL(vaddr)) {
		rc = vaddr ? PTR_ERR(vaddr) : -ENOMEM;
		goto end_cpu_access;
	}
	fujisan_fill_test_pattern(vaddr);
	dma_buf_kunmap(buffer->dma_buf, 0, vaddr);
	if (cpu_sync)
		dma_buf_end_cpu_access(buffer->dma_buf, 0, FUJISAN_TEST_SIZE,
			DMA_BIDIRECTIONAL);

	/* dma-buf owns the allocation after it has been shared. */
	ion_free(client, handle);
	return 0;

end_cpu_access:
	if (cpu_sync)
		dma_buf_end_cpu_access(buffer->dma_buf, 0, FUJISAN_TEST_SIZE,
			DMA_BIDIRECTIONAL);
unmap_iova:
	mdss_smmu_unmap_dma_buf(buffer->table, MDSS_IOMMU_DOMAIN_UNSECURE,
		DMA_BIDIRECTIONAL, buffer->dma_buf);
unmap_attachment:
	dma_buf_unmap_attachment(buffer->attachment, buffer->table,
		DMA_BIDIRECTIONAL);
detach_dma_buf:
	dma_buf_detach(buffer->dma_buf, buffer->attachment);
put_dma_buf:
	dma_buf_put(buffer->dma_buf);
	buffer->dma_buf = NULL;
free_handle:
	ion_free(client, handle);
	memset(buffer, 0, sizeof(*buffer));
	return rc;
}

static void fujisan_free_test_buffer(void)
{
	struct fujisan_composite_buffer *buffer = &fujisan_composite.buffer;

	if (!buffer->dma_buf)
		return;

	mdss_smmu_unmap_dma_buf(buffer->table, MDSS_IOMMU_DOMAIN_UNSECURE,
		DMA_BIDIRECTIONAL, buffer->dma_buf);
	dma_buf_unmap_attachment(buffer->attachment, buffer->table,
		DMA_BIDIRECTIONAL);
	dma_buf_detach(buffer->dma_buf, buffer->attachment);
	dma_buf_put(buffer->dma_buf);
	memset(buffer, 0, sizeof(*buffer));
}

static void fujisan_destroy_test_pipe(struct msm_fb_data_type *mfd,
	struct mdss_mdp_pipe *pipe, struct mdss_mdp_data *data)
{
	struct mdss_overlay_private *mdp = mfd_to_mdp5_data(mfd);

	if (!pipe)
		return;

	mutex_lock(&mdp->list_lock);
	mdss_mdp_mixer_pipe_unstage(pipe, pipe->mixer_left);
	mdss_mdp_mixer_pipe_unstage(pipe, pipe->mixer_right);
	if (!list_empty(&pipe->list))
		list_del_init(&pipe->list);
	if (data)
		mdss_mdp_overlay_buf_free(mfd, data);
	mdss_mdp_pipe_destroy(pipe);
	mutex_unlock(&mdp->list_lock);
}

static int fujisan_add_test_pipe(struct msm_fb_data_type *mfd, u32 src_x,
	u32 dst_x, u32 dst_y, struct mdss_mdp_pipe **pipe_out,
	struct mdss_mdp_data **data_out)
{
	struct fujisan_composite_buffer *buffer = &fujisan_composite.buffer;
	struct mdss_overlay_private *mdp = mfd_to_mdp5_data(mfd);
	struct mdss_mdp_pipe *pipe;
	struct mdss_mdp_data *data;
	struct mdp_overlay req;
	int rc;

	memset(&req, 0, sizeof(req));
	req.src.width = FUJISAN_WIDE_WIDTH;
	req.src.height = FUJISAN_WIDE_HEIGHT;
	req.src.format = MDP_RGBA_8888;
	req.src_rect.x = src_x;
	req.src_rect.y = 0;
	req.src_rect.w = FUJISAN_SPLIT_X;
	req.src_rect.h = FUJISAN_WIDE_HEIGHT;
	req.dst_rect.x = dst_x;
	req.dst_rect.y = dst_y;
	req.dst_rect.w = FUJISAN_SPLIT_X;
	req.dst_rect.h = FUJISAN_WIDE_HEIGHT;
	req.z_order = MDSS_MDP_STAGE_3;
	req.is_fg = 1;
	req.alpha = 0xff;
	req.blend_op = BLEND_OP_OPAQUE;
	req.pipe_type = PIPE_TYPE_VIG;
	req.id = MSMFB_NEW_REQUEST;

	rc = mdss_mdp_overlay_pipe_setup(mfd, &req, &pipe, NULL, true);
	if (rc)
		return rc;

	rc = mdss_mdp_pipe_map(pipe);
	if (rc)
		goto release_pipe;

	mutex_lock(&mdp->list_lock);
	data = mdss_mdp_overlay_buf_alloc(mfd, pipe);
	mutex_unlock(&mdp->list_lock);
	if (!data) {
		rc = -ENOMEM;
		goto unmap_pipe;
	}

	data->p[0].addr = buffer->iova;
	data->p[0].len = buffer->size;
	data->num_planes = 1;
	mdss_mdp_pipe_unmap(pipe);

	*pipe_out = pipe;
	*data_out = data;
	return 0;

unmap_pipe:
	mdss_mdp_pipe_unmap(pipe);
release_pipe:
	fujisan_destroy_test_pipe(mfd, pipe, NULL);
	return rc;
}

static void fujisan_drop_test_pipe(struct msm_fb_data_type *mfd,
	struct mdss_mdp_pipe **pipe_ptr, struct mdss_mdp_data **data_ptr)
{
	struct mdss_mdp_pipe *pipe = *pipe_ptr;
	struct mdss_mdp_data *data = *data_ptr;

	if (!pipe)
		return;

	fujisan_destroy_test_pipe(mfd, pipe, data);
	*pipe_ptr = NULL;
	*data_ptr = NULL;
}

static int fujisan_submit_pair(void)
{
	struct fujisan_composite_link *link = &fujisan_composite.link;
	int rc;

	fujisan_trace("wide_diag_submit_begin", 0);
	rc = mdss_mdp_pipe_queue_data(fujisan_composite.left_pipe,
		fujisan_composite.left_data);
	if (rc)
		goto error;
	fujisan_trace("wide_diag_b_queue", 0);

	rc = mdss_mdp_pipe_queue_data(fujisan_composite.right_pipe,
		fujisan_composite.right_data);
	if (rc)
		goto error;
	fujisan_trace("wide_diag_a_queue", 0);

	/*
	 * These command contexts started independently.  Do not fabricate a live
	 * split relationship: submit each native CTL directly for the crop/scanout
	 * diagnostic.  A true C transaction must be created before either command
	 * context starts and is still required for wide-primary completion.
	 */
	rc = mdss_mdp_display_commit(link->left_ctl, NULL, NULL);
	if (rc)
		goto error;
	fujisan_trace("wide_diag_b_kickoff", 0);
	rc = mdss_mdp_display_commit(link->right_ctl, NULL, NULL);
	if (rc)
		goto error;
	fujisan_trace("wide_diag_a_kickoff", 0);
	rc = mdss_mdp_display_wait4pingpong(link->left_ctl, true);
	if (rc)
		goto error;
	rc = mdss_mdp_display_wait4pingpong(link->right_ctl, true);
	if (rc)
		goto error;

	fujisan_trace("wide_diag_submit_end", 0);
	return 0;

error:
	fujisan_trace("wide_error", rc);
	return rc;
}

static int fujisan_submit_native(void)
{
	struct fujisan_composite_link *link = &fujisan_composite.link;
	int rc;

	fujisan_trace_ctl("wide_submit_begin", link->left_ctl, 0);
	rc = mdss_mdp_pipe_queue_data(fujisan_composite.left_pipe,
		fujisan_composite.left_data);
	if (rc)
		goto error;
	rc = mdss_mdp_pipe_queue_data(fujisan_composite.right_pipe,
		fujisan_composite.right_data);
	if (rc)
		goto error;

	/* One native master commit configures both mixers, flushes both CTLs,
	 * and enters the dual-display command transaction. */
	rc = mdss_mdp_display_commit(link->left_ctl, NULL, NULL);
	if (rc)
		goto error;
	fujisan_trace_ctl("wide_native_a_armed", link->right_ctl, 0);
	fujisan_trace_ctl("wide_native_b_armed", link->left_ctl, 0);
	rc = mdss_mdp_display_wait4pingpong(link->left_ctl, true);
	if (rc)
		goto error;
	fujisan_trace_ctl("wide_submit_end", link->left_ctl, 0);
	return 0;

error:
	fujisan_trace_ctl("wide_error", link->left_ctl, rc);
	return rc;
}

static int fujisan_start_native_boot_test(void)
{
	struct msm_fb_data_type *mfd;
	const char *stage = "wide_native_topology";
	int rc;

	if (fujisan_composite.active || fujisan_composite.link.linked)
		return -EBUSY;

	rc = fujisan_get_native_mfd(&mfd);
	if (rc)
		goto error;

	stage = "wide_native_claim";
	rc = fujisan_claim_native_scanout(mfd);
	if (rc)
		goto unlink_error;

	if (!mfd_to_ctl(mfd)) {
		stage = "wide_native_start";
		if (!mfd->mdp.on_fnc) {
			rc = -ENODEV;
			goto release;
		}
		/* This is the normal MDSS fb-on path.  It creates the already
		 * declared master/slave CTLs; it does not re-parent live CTLs. */
		/* on_fnc() may call mdss_mdp_overlay_off() on failure, which takes
		 * ov_lock itself.  It must therefore run before this test owns that
		 * lock; otherwise its normal cleanup self-deadlocks. */
		rc = mfd->mdp.on_fnc(mfd);
		if (rc)
			goto release;
	}

	fujisan_lock_ov_one(mfd);

	stage = "wide_native_topology";
	rc = fujisan_link_native_ctls(mfd);
	if (rc)
		goto unlock_release;

	stage = "wide_native_iommu";
	rc = mdss_iommu_ctrl(1);
	if (IS_ERR_VALUE(rc))
		goto unlock_release;
	fujisan_composite.iommu_attached = true;

	stage = "wide_native_buffer";
	rc = fujisan_alloc_test_buffer(mfd);
	if (rc)
		goto cleanup;

	/* fb0/ctl0 is physical B, so C's right half feeds the master/left
	 * mixer.  The native slave is physical A and receives C's left half on
	 * the right mixer at global destination x=1080. */
	stage = "wide_native_pipe_b";
	rc = fujisan_add_test_pipe(mfd, FUJISAN_SPLIT_X, 0, FUJISAN_B_PAD_TOP,
		&fujisan_composite.left_pipe, &fujisan_composite.left_data);
	if (rc)
		goto cleanup;
	stage = "wide_native_pipe_a";
	rc = fujisan_add_test_pipe(mfd, 0, FUJISAN_SPLIT_X, 0,
		&fujisan_composite.right_pipe, &fujisan_composite.right_data);
	if (rc)
		goto cleanup;

	stage = "wide_native_submit";
	rc = fujisan_submit_native();
	if (rc)
		goto cleanup;

	fujisan_composite.submit_seq++;
	fujisan_composite.active = true;
	fujisan_composite.last_error = 0;
	fujisan_composite.last_stage = "wide_native_active";
	fujisan_unlock_ov_one(mfd);
	return 0;

cleanup:
	fujisan_drop_test_pipe(mfd, &fujisan_composite.right_pipe,
		&fujisan_composite.right_data);
	fujisan_drop_test_pipe(mfd, &fujisan_composite.left_pipe,
		&fujisan_composite.left_data);
	fujisan_free_test_buffer();
	if (fujisan_composite.iommu_attached) {
		mdss_iommu_ctrl(0);
		fujisan_composite.iommu_attached = false;
	}
unlock_release:
	fujisan_unlock_ov_one(mfd);
release:
	fujisan_release_native_scanout(mfd);
unlink_error:
	fujisan_unlink_ctls();
error:
	fujisan_composite.last_stage = stage;
	fujisan_composite.last_error = rc;
	fujisan_trace_ctl(stage, fujisan_composite.link.left_ctl, rc);
	pr_err("%s failed rc=%d\n", stage, rc);
	return rc;
}

static int fujisan_start_test(void)
{
	struct msm_fb_data_type *left;
	struct msm_fb_data_type *right;
	const char *stage;
	int rc;

	if (fujisan_composite.active || fujisan_composite.link.linked)
		return -EBUSY;

	stage = "wide_get_fbs";
	rc = fujisan_get_mfds(&left, &right);
	if (rc) {
		fujisan_composite.last_stage = stage;
		fujisan_composite.last_error = rc;
		fujisan_trace(stage, rc);
		return rc;
	}
	stage = "wide_claim";
	rc = fujisan_claim_scanout(left, right);
	if (rc) {
		fujisan_composite.last_stage = stage;
		fujisan_composite.last_error = rc;
		fujisan_trace(stage, rc);
		return rc;
	}
	stage = "wide_power_b";
	rc = fujisan_prepare_secondary(right);
	if (rc)
		goto release;

	fujisan_lock_ov(left, right);
	stage = "wide_start_a";
	rc = mdss_mdp_overlay_start(left);
	if (rc)
		goto unlock_release;
	stage = "wide_start_b";
	rc = mdss_mdp_overlay_start(right);
	if (rc)
		goto unlock_release;
	stage = "wide_iommu";
	rc = mdss_iommu_ctrl(1);
	if (IS_ERR_VALUE(rc))
		goto unlock_release;
	fujisan_composite.iommu_attached = true;
	stage = "wide_link";
	rc = fujisan_link_ctls(left, right);
	if (rc)
		goto unlock_release;
	stage = "wide_buffer";
	rc = fujisan_alloc_test_buffer(left);
	if (rc)
		goto cleanup;
	/* Hardware registration order is B (fb0) then A (fb1).  Keep C's
	 * logical geometry stable: source left goes to physical A, source right
	 * goes to physical B. */
	stage = "wide_pipe_b";
	rc = fujisan_add_test_pipe(left, FUJISAN_SPLIT_X, 0, FUJISAN_B_PAD_TOP,
		&fujisan_composite.left_pipe, &fujisan_composite.left_data);
	if (rc)
		goto cleanup;
	stage = "wide_pipe_a";
	rc = fujisan_add_test_pipe(right, 0, 0, 0, &fujisan_composite.right_pipe,
		&fujisan_composite.right_data);
	if (rc)
		goto cleanup;

	stage = "wide_submit";
	rc = fujisan_submit_pair();
	if (rc)
		goto cleanup;

	fujisan_composite.submit_seq++;
	fujisan_composite.active = true;
	fujisan_composite.last_error = 0;
	fujisan_composite.last_stage = "wide_active";
	fujisan_unlock_ov(left, right);
	return 0;

cleanup:
	fujisan_drop_test_pipe(right, &fujisan_composite.right_pipe,
		&fujisan_composite.right_data);
	fujisan_drop_test_pipe(left, &fujisan_composite.left_pipe,
		&fujisan_composite.left_data);
	fujisan_free_test_buffer();
	fujisan_unlink_ctls();
	if (fujisan_composite.iommu_attached) {
		mdss_iommu_ctrl(0);
		fujisan_composite.iommu_attached = false;
	}
unlock_release:
	fujisan_unlock_ov(left, right);
release:
	fujisan_release_scanout(left, right);
	fujisan_composite.last_stage = stage;
	fujisan_composite.last_error = rc;
	fujisan_trace(stage, rc);
	pr_err("%s failed rc=%d\n", stage, rc);
	return rc;

}

static void fujisan_unstage_test_pipe(struct mdss_mdp_pipe *pipe)
{
	if (!pipe)
		return;

	mdss_mdp_mixer_pipe_unstage(pipe, pipe->mixer_left);
	mdss_mdp_mixer_pipe_unstage(pipe, pipe->mixer_right);
}

static int fujisan_stop_test(void)
{
	struct fujisan_composite_link *link = &fujisan_composite.link;
	struct msm_fb_data_type *left;
	struct msm_fb_data_type *right;
	bool native;
	int rc = 0;

	if (!fujisan_composite.active || !link->linked)
		return -EINVAL;

	left = link->left_mfd;
	right = link->right_mfd;
	native = link->native_topology;
	if (native)
		fujisan_lock_ov_one(left);
	else
		fujisan_lock_ov(left, right);

	fujisan_unstage_test_pipe(fujisan_composite.left_pipe);
	fujisan_unstage_test_pipe(fujisan_composite.right_pipe);
	rc = mdss_mdp_display_commit(link->left_ctl, NULL, NULL);
	if (!rc && !native)
		rc = mdss_mdp_display_commit(link->right_ctl, NULL, NULL);
	if (!rc)
		rc = mdss_mdp_display_wait4pingpong(link->left_ctl, true);
	if (!rc && !native)
		rc = mdss_mdp_display_wait4pingpong(link->right_ctl, true);
	if (rc)
		fujisan_trace("wide_error", rc);

	/* Let both command-mode completion/resource callbacks leave the bridge. */
	msleep(100);
	fujisan_drop_test_pipe(native ? left : right,
		&fujisan_composite.right_pipe,
		&fujisan_composite.right_data);
	fujisan_drop_test_pipe(left, &fujisan_composite.left_pipe,
		&fujisan_composite.left_data);
	fujisan_free_test_buffer();
	if (fujisan_composite.iommu_attached) {
		mdss_iommu_ctrl(0);
		fujisan_composite.iommu_attached = false;
	}
	if (native) {
		fujisan_unlock_ov_one(left);
		fujisan_release_native_scanout(left);
	} else {
		fujisan_unlock_ov(left, right);
		fujisan_release_scanout(left, right);
	}
	fujisan_unlink_ctls();
	fujisan_composite.active = false;
	fujisan_composite.last_error = rc;
	fujisan_composite.last_stage = rc ? "wide_stop_error" : "idle";
	return rc;
}

static int fujisan_mode_show(struct seq_file *s, void *unused)
{
	mutex_lock(&fujisan_composite.lock);
	seq_puts(s, fujisan_composite.active ? "wide-test\n" : "idle\n");
	mutex_unlock(&fujisan_composite.lock);
	return 0;
}

static int fujisan_last_submit_show(struct seq_file *s, void *unused)
{
	mutex_lock(&fujisan_composite.lock);
	seq_printf(s, "seq=%llu active=%d stage=%s buffer=%ux%u bytes=%lu rgba-linear split=%u %s\n",
		fujisan_composite.submit_seq, fujisan_composite.active,
		fujisan_composite.last_stage,
		FUJISAN_WIDE_WIDTH, FUJISAN_WIDE_HEIGHT,
		fujisan_composite.buffer.size,
		FUJISAN_SPLIT_X,
		fujisan_composite.link.native_topology ?
		"native-dual-ctl-one-shot" : "independent-ctl-diagnostic");
	mutex_unlock(&fujisan_composite.lock);
	return 0;
}

static int fujisan_last_error_show(struct seq_file *s, void *unused)
{
	mutex_lock(&fujisan_composite.lock);
	seq_printf(s, "%d\n", fujisan_composite.last_error);
	mutex_unlock(&fujisan_composite.lock);
	return 0;
}

static int fujisan_no_fence_show(struct seq_file *s, void *unused)
{
	seq_puts(s, "stage1: synchronous pingpong wait; retire fence is phase2\n");
	return 0;
}

static int fujisan_mode_open(struct inode *inode, struct file *file)
{
	return single_open(file, fujisan_mode_show, inode->i_private);
}

static int fujisan_last_submit_open(struct inode *inode, struct file *file)
{
	return single_open(file, fujisan_last_submit_show, inode->i_private);
}

static int fujisan_last_error_open(struct inode *inode, struct file *file)
{
	return single_open(file, fujisan_last_error_show, inode->i_private);
}

static int fujisan_no_fence_open(struct inode *inode, struct file *file)
{
	return single_open(file, fujisan_no_fence_show, inode->i_private);
}

static ssize_t fujisan_test_write(struct file *file, const char __user *user,
	size_t count, loff_t *ppos)
{
	char command[16];
	int rc;

	if (!count || count >= sizeof(command))
		return -EINVAL;
	if (copy_from_user(command, user, count))
		return -EFAULT;
	command[count] = '\0';
	strim(command);

	mutex_lock(&fujisan_composite.lock);
	if (!strcmp(command, "run"))
		rc = fujisan_start_test();
	else if (!strcmp(command, "run-native"))
		rc = fujisan_start_native_boot_test();
	else if (!strcmp(command, "stop"))
		rc = fujisan_stop_test();
	else
		rc = -EINVAL;
	mutex_unlock(&fujisan_composite.lock);

	return rc ? rc : count;
}

static const struct file_operations fujisan_mode_fops = {
	.open = fujisan_mode_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static const struct file_operations fujisan_last_submit_fops = {
	.open = fujisan_last_submit_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static const struct file_operations fujisan_last_error_fops = {
	.open = fujisan_last_error_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static const struct file_operations fujisan_no_fence_fops = {
	.open = fujisan_no_fence_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static const struct file_operations fujisan_test_fops = {
	.open = simple_open,
	.write = fujisan_test_write,
	.llseek = no_llseek,
};

static int __init fujisan_composite_debugfs_init(void)
{
	fujisan_composite.debug_root = debugfs_create_dir("fujisan_display", NULL);
	if (IS_ERR_OR_NULL(fujisan_composite.debug_root))
		return 0;

	debugfs_create_file("mode", 0444, fujisan_composite.debug_root, NULL,
		&fujisan_mode_fops);
	debugfs_create_file("last_submit", 0444, fujisan_composite.debug_root,
		NULL, &fujisan_last_submit_fops);
	debugfs_create_file("last_error", 0444, fujisan_composite.debug_root,
		NULL, &fujisan_last_error_fops);
	debugfs_create_file("a_retire_fence", 0444, fujisan_composite.debug_root,
		NULL, &fujisan_no_fence_fops);
	debugfs_create_file("b_retire_fence", 0444, fujisan_composite.debug_root,
		NULL, &fujisan_no_fence_fops);
	debugfs_create_file("test", 0200, fujisan_composite.debug_root, NULL,
		&fujisan_test_fops);
	return 0;
}
late_initcall(fujisan_composite_debugfs_init);
