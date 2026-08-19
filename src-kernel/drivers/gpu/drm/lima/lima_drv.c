// SPDX-License-Identifier: GPL-2.0 OR MIT
/* Copyright 2017-2019 Qiang Yu <yuq825@gmail.com> */
/*
 * 3.10 backport of lima_drv.c.
 *
 * Notable deltas vs upstream v5.2:
 *  - 3.10 drm core has no render nodes and no DRM_RENDER_ALLOW: the
 *    ioctls run DRM_UNLOCKED and without DRM_AUTH on the primary node
 *    (same trust model as other 3.10-era GPU drivers; documented in
 *    docs/gap-analysis.md).
 *  - driver_features = DRIVER_GEM | DRIVER_PRIME only (3.10 core).
 *  - .load/.unload via drm_platform_init/exit (no drm_dev_alloc in 3.10).
 *  - probe: OF match ("arm,mali-400"/"arm,mali-450") -> platform_data ->
 *    default mali400.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include <drm/drmP.h>
#include <drm/lima_drm.h>

#include "lima_compat.h"
#include "lima_drv.h"
#include "lima_gem.h"
#include "lima_gem_prime.h"
#include "lima_device.h"
#include "lima_platform.h"
#include "lima_sched.h"
#include "lima_vm.h"

static int lima_ioctl_get_param(struct drm_device *dev, void *data,
				struct drm_file *file)
{
	struct lima_device *ldev = to_lima_dev(dev);
	struct drm_lima_get_param *args = data;

	switch (args->param) {
	case DRM_LIMA_PARAM_GPU_ID:
		args->value = ldev->id == lima_gpu_mali400 ?
			DRM_LIMA_PARAM_GPU_ID_MALI400 :
			DRM_LIMA_PARAM_GPU_ID_MALI450;
		break;
	case DRM_LIMA_PARAM_NUM_PP:
		args->value = ldev->num_pp;
		break;
	case DRM_LIMA_PARAM_GP_VERSION:
		args->value = ldev->gp_version;
		break;
	case DRM_LIMA_PARAM_PP_VERSION:
		args->value = ldev->pp_version;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int lima_ioctl_gem_create(struct drm_device *dev, void *data,
				 struct drm_file *file)
{
	struct drm_lima_gem_create *args = data;

	if (args->flags || args->pad)
		return -EINVAL;
	if (args->size == 0)
		return -EINVAL;

	return lima_gem_create_handle(dev, file, args->size, args->flags,
				      &args->handle);
}

static int lima_ioctl_gem_info(struct drm_device *dev, void *data,
			       struct drm_file *file)
{
	struct drm_lima_gem_info *args = data;

	return lima_gem_get_info(file, args->handle, &args->va, &args->offset);
}

static int lima_ioctl_gem_submit(struct drm_device *dev, void *data,
				 struct drm_file *file)
{
	struct lima_device *ldev = to_lima_dev(dev);
	struct lima_drm_priv *priv = to_lima_drm_priv(file);
	struct drm_lima_gem_submit *args = data;
	struct lima_sched_pipe *pipe;
	struct lima_submit submit = {0};
	int err;

	if (args->pipe >= lima_pipe_num || args->nr_bos == 0)
		return -EINVAL;
	if (args->flags & ~LIMA_SUBMIT_FLAG_EXPLICIT_FENCE)
		return -EINVAL;

	pipe = ldev->pipe + args->pipe;
	if (args->frame_size != pipe->frame_size)
		return -EINVAL;

	submit.ctx = lima_ctx_get(&priv->ctx_mgr, args->ctx);
	if (!submit.ctx)
		return -EINVAL;
	submit.pipe = args->pipe;
	submit.flags = args->flags;
	submit.out_sync = args->out_sync;
	submit.in_sync[0] = args->in_sync[0];
	submit.in_sync[1] = args->in_sync[1];
	submit.nr_bos = args->nr_bos;

	submit.bos = kcalloc(args->nr_bos, sizeof(*submit.bos), GFP_KERNEL);
	if (!submit.bos) {
		err = -ENOMEM;
		goto out_ctx;
	}
	if (copy_from_user(submit.bos, u64_to_user_ptr(args->bos),
			   args->nr_bos * sizeof(*submit.bos))) {
		err = -EFAULT;
		goto out_bos;
	}

	/* frame lives right after the task in the pipe's task slab */
	submit.task = kmem_cache_zalloc(pipe->task_slab, GFP_KERNEL);
	if (!submit.task) {
		err = -ENOMEM;
		goto out_bos;
	}
	submit.task->frame = submit.task + 1;
	if (copy_from_user(submit.task->frame, u64_to_user_ptr(args->frame),
			   args->frame_size)) {
		err = -EFAULT;
		goto out_task;
	}

	err = lima_gem_submit(file, &submit);
	/* on success the pipe owns task (freed from the slab at completion)
	 * and the ctx ref (dropped by lima_sched_task_fini) */

out_task:
	if (err)
		kmem_cache_free(pipe->task_slab, submit.task);
out_bos:
	kfree(submit.bos);
out_ctx:
	if (err)
		lima_ctx_put(submit.ctx);
	return err;
}

static int lima_ioctl_gem_wait(struct drm_device *dev, void *data,
			       struct drm_file *file)
{
	struct drm_lima_gem_wait *args = data;

	return lima_gem_wait(file, args->handle, args->op, args->timeout_ns);
}

static int lima_ioctl_ctx_create(struct drm_device *dev, void *data,
				 struct drm_file *file)
{
	struct lima_drm_priv *priv = to_lima_drm_priv(file);
	struct drm_lima_ctx_create *args = data;

	if (args->_pad)
		return -EINVAL;
	return lima_ctx_create(to_lima_dev(dev), &priv->ctx_mgr, &args->id);
}

static int lima_ioctl_ctx_free(struct drm_device *dev, void *data,
			       struct drm_file *file)
{
	struct lima_drm_priv *priv = to_lima_drm_priv(file);
	struct drm_lima_ctx_free *args = data;

	if (args->_pad)
		return -EINVAL;
	return lima_ctx_free(&priv->ctx_mgr, args->id);
}

static struct drm_ioctl_desc lima_ioctls[] = {
	DRM_IOCTL_DEF_DRV(LIMA_GET_PARAM, lima_ioctl_get_param, DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(LIMA_GEM_CREATE, lima_ioctl_gem_create, DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(LIMA_GEM_INFO, lima_ioctl_gem_info, DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(LIMA_GEM_SUBMIT, lima_ioctl_gem_submit, DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(LIMA_GEM_WAIT, lima_ioctl_gem_wait, DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(LIMA_CTX_CREATE, lima_ioctl_ctx_create, DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(LIMA_CTX_FREE, lima_ioctl_ctx_free, DRM_UNLOCKED),
};

static int lima_drm_open(struct drm_device *dev, struct drm_file *file)
{
	struct lima_device *ldev = to_lima_dev(dev);
	struct lima_drm_priv *priv;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->vm = lima_vm_create(ldev);
	if (!priv->vm) {
		kfree(priv);
		return -ENOMEM;
	}
	lima_ctx_mgr_init(&priv->ctx_mgr);
	file->driver_priv = priv;
	return 0;
}

static void lima_drm_postclose(struct drm_device *dev, struct drm_file *file)
{
	struct lima_drm_priv *priv = to_lima_drm_priv(file);

	lima_ctx_mgr_fini(&priv->ctx_mgr);	/* drains contexts (waits) */
	lima_vm_put(priv->vm);
	kfree(priv);
}

static const struct file_operations lima_drm_driver_fops = {
	.owner = THIS_MODULE,
	.open = drm_open,
	.release = drm_release,
	.unlocked_ioctl = drm_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = drm_compat_ioctl,
#endif
	.poll = drm_poll,
	.read = drm_read,
	.llseek = no_llseek,
	.mmap = lima_gem_mmap,
};

static const struct of_device_id lima_dt_match[] = {
	{ .compatible = "arm,mali-400",
	  .data = (void *)lima_gpu_mali400 },
	{ .compatible = "arm,mali-450",
	  .data = (void *)lima_gpu_mali450 },
	{ }
};
MODULE_DEVICE_TABLE(of, lima_dt_match);

static int lima_drm_load(struct drm_device *dev, unsigned long flags)
{
	struct platform_device *pdev = to_platform_device(dev->dev);
	struct lima_device *ldev = platform_get_drvdata(pdev);
	int err;

	dev->dev_private = ldev;
	ldev->ddev = dev;

	err = lima_device_init(ldev);
	if (err)
		return err;

	dev_info(dev->dev, "lima: device up, %d pp cores\n", ldev->num_pp);
	return 0;
}

static int lima_drm_unload(struct drm_device *dev)
{
	lima_device_fini(to_lima_dev(dev));
	return 0;
}

static struct drm_driver lima_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_PRIME,
	.load = lima_drm_load,
	.unload = lima_drm_unload,
	.open = lima_drm_open,
	.postclose = lima_drm_postclose,
	.ioctls = lima_ioctls,
	.num_ioctls = ARRAY_SIZE(lima_ioctls),
	.fops = &lima_drm_driver_fops,

	.gem_free_object = lima_gem_free_object,
	.gem_open_object = lima_gem_object_open,
	.gem_close_object = lima_gem_object_close,
	.gem_vm_ops = &lima_gem_vm_ops,

	.gem_prime_import_sg_table = lima_gem_prime_import_sg_table,
	.gem_prime_get_sg_table = lima_gem_prime_get_sg_table,

	.name = "lima",
	.desc = "lima DRM (3.10 backport)",
	.date = "20190217",
	.major = 1,
	.minor = 0,
	.patchlevel = 0,
};

static int lima_pdev_probe(struct platform_device *pdev)
{
	const struct of_device_id *match;
	struct lima_platform_data *pdata = pdev->dev.platform_data;
	struct lima_device *ldev;
	int err;

	ldev = kzalloc(sizeof(*ldev), GFP_KERNEL);
	if (!ldev)
		return -ENOMEM;

	ldev->id = lima_gpu_mali400;
	match = of_match_node(lima_dt_match, pdev->dev.of_node);
	if (match && match->data)
		ldev->id = (enum lima_gpu_id)match->data;
	if (pdata)
		ldev->id = pdata->gpu_id;
	ldev->pdata = pdata;

	ldev->pdev = pdev;
	ldev->dev = &pdev->dev;
	platform_set_drvdata(pdev, ldev);

	err = drm_platform_init(&lima_drm_driver, pdev);
	if (err) {
		dev_err(&pdev->dev, "lima: drm_platform_init failed %d\n", err);
		kfree(ldev);
		return err;
	}
	return 0;
}

static int lima_pdev_remove(struct platform_device *pdev)
{
	struct lima_device *ldev = platform_get_drvdata(pdev);

	/* calls lima_drm_unload -> lima_device_fini, then tears down the
	 * drm device (3.10 core does both inside drm_platform_exit) */
	drm_platform_exit(&lima_drm_driver, pdev);
	kfree(ldev);
	return 0;
}

static struct platform_driver lima_platform_driver = {
	.probe = lima_pdev_probe,
	.remove = lima_pdev_remove,
	.driver = {
		.name = "lima",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(lima_dt_match),
	},
};

static int __init lima_init(void)
{
	int ret;

	ret = lima_sched_slab_init();
	if (ret)
		return ret;

	ret = platform_driver_register(&lima_platform_driver);
	if (ret)
		lima_sched_slab_fini();
	return ret;
}

static void __exit lima_exit(void)
{
	platform_driver_unregister(&lima_platform_driver);
	lima_sched_slab_fini();
}

module_init(lima_init);
module_exit(lima_exit);

MODULE_AUTHOR("Qiang Yu <yuq825@gmail.com> (upstream), 3.10 backport");
MODULE_DESCRIPTION("lima DRM driver for ARM Mali400/450 (Linux 3.10 backport)");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:lima");
