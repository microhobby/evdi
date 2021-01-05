// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2012 Red Hat
 * Copyright (c) 2015 - 2020 DisplayLink (UK) Ltd.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License v2. See the file COPYING in the main directory of this archive for
 * more details.
 */

#include <linux/version.h>
#if KERNEL_VERSION(5, 5, 0) <= LINUX_VERSION_CODE || defined(EL8)
#else
#include <drm/drmP.h>
#endif
#include <drm/drm_crtc_helper.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#if KERNEL_VERSION(5, 9, 0) <= LINUX_VERSION_CODE
#include <linux/iommu.h>
#endif

#include "evdi_drv.h"
#include "evdi_drm.h"
#include "evdi_params.h"
#include "evdi_debug.h"

MODULE_AUTHOR("DisplayLink (UK) Ltd.");
MODULE_DESCRIPTION("Extensible Virtual Display Interface");
MODULE_LICENSE("GPL");

#define EVDI_DEVICE_COUNT_MAX 16

static struct evdi_context {
	struct device *root_dev;
	unsigned int dev_count;
	struct platform_device *devices[EVDI_DEVICE_COUNT_MAX];
} evdi_context;

static struct drm_driver driver;

struct drm_ioctl_desc evdi_painter_ioctls[] = {
	DRM_IOCTL_DEF_DRV(EVDI_CONNECT, evdi_painter_connect_ioctl,
			  DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(EVDI_REQUEST_UPDATE,
			  evdi_painter_request_update_ioctl, DRM_UNLOCKED),
	DRM_IOCTL_DEF_DRV(EVDI_GRABPIX, evdi_painter_grabpix_ioctl,
			  DRM_UNLOCKED),
};

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 11, 0)
static const struct vm_operations_struct evdi_gem_vm_ops = {
	.fault = evdi_gem_fault,
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};
#endif

static const struct file_operations evdi_driver_fops = {
	.owner = THIS_MODULE,
	.open = drm_open,
	.mmap = evdi_drm_gem_mmap,
	.poll = drm_poll,
	.read = drm_read,
	.unlocked_ioctl = drm_ioctl,
	.release = drm_release,
#ifdef CONFIG_COMPAT
	.compat_ioctl = evdi_compat_ioctl,
#endif
	.llseek = noop_llseek,
};

static int evdi_enable_vblank(__always_unused struct drm_device *dev,
			      __always_unused unsigned int pipe)
{
	return 1;
}

static void evdi_disable_vblank(__always_unused struct drm_device *dev,
				__always_unused unsigned int pipe)
{
}

static struct drm_driver driver = {
#if KERNEL_VERSION(5, 4, 0) <= LINUX_VERSION_CODE || defined(EL8)
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
#else
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_PRIME
			 | DRIVER_ATOMIC,
#endif
	.unload = evdi_driver_unload,
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 11, 0)
	.preclose = evdi_driver_preclose,
#endif

	.postclose = evdi_driver_postclose,

	/* gem hooks */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 9, 0)
	// In 5.9 and below we have gem_free_object
	.gem_free_object = evdi_gem_free_object,
#elsif LINUX_VERSION_CODE <= KERNEL_VERSION(5, 11, 0)
	// In 5.9 and 5.10 this is called gem_free_object_unlocked
	.gem_free_object_unlocked = evdi_gem_free_object,
	// Note that gem_free_object_unlocked no longer exists in 5.11 - it needs to be added to the gem object instead
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 11, 0)
    // In 5.11+, this is set in the object instance
	.gem_vm_ops = &evdi_gem_vm_ops,
#endif


	.dumb_create = evdi_dumb_create,
	.dumb_map_offset = evdi_gem_mmap,
	.dumb_destroy = drm_gem_dumb_destroy,

	.ioctls = evdi_painter_ioctls,
	.num_ioctls = ARRAY_SIZE(evdi_painter_ioctls),

	.fops = &evdi_driver_fops,

	.prime_fd_to_handle = drm_gem_prime_fd_to_handle,
	.gem_prime_import = drm_gem_prime_import,
	.prime_handle_to_fd = drm_gem_prime_handle_to_fd,
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 11, 0)
	// In kernel 5.11, these have been moved to the object instance
	.gem_prime_export = drm_gem_prime_export,
	.gem_prime_get_sg_table = evdi_prime_get_sg_table,
#endif
	.gem_prime_import_sg_table = evdi_prime_import_sg_table,

	.enable_vblank = evdi_enable_vblank,
	.disable_vblank = evdi_disable_vblank,

	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.date = DRIVER_DATE,
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
	.patchlevel = DRIVER_PATCH,
};

static void evdi_add_device(void)
{
	struct platform_device_info pdevinfo = {
		.parent = NULL,
		.name = "evdi",
		.id = evdi_context.dev_count,
		.res = NULL,
		.num_res = 0,
		.data = NULL,
		.size_data = 0,
		.dma_mask = DMA_BIT_MASK(32),
	};

	evdi_context.devices[evdi_context.dev_count] =
	    platform_device_register_full(&pdevinfo);
	if (dma_set_mask(&evdi_context.devices[evdi_context.dev_count]->dev,
			 DMA_BIT_MASK(64))) {
		EVDI_DEBUG("Unable to change dma mask to 64 bit. ");
		EVDI_DEBUG("Sticking with 32 bit\n");
	}
	evdi_context.dev_count++;
}

static int evdi_add_devices(unsigned int val)
{
	if (val == 0) {
		EVDI_WARN("Adding 0 devices has no effect\n");
		return 0;
	}
	if (val > EVDI_DEVICE_COUNT_MAX - evdi_context.dev_count) {
		EVDI_ERROR("Evdi device add failed. Too many devices.\n");
		return -EINVAL;
	}

	EVDI_DEBUG("Increasing device count to %u\n",
		   evdi_context.dev_count + val);
	while (val--)
		evdi_add_device();
	return 0;
}

static int evdi_platform_probe(struct platform_device *pdev)
{
	struct drm_device *dev;
	int ret;
#if KERNEL_VERSION(5, 9, 0) <= LINUX_VERSION_CODE
#if IS_ENABLED(CONFIG_IOMMU_API) && defined(CONFIG_INTEL_IOMMU)
	struct dev_iommu iommu;
#endif
#endif
	EVDI_CHECKPT();

/* Intel-IOMMU workaround: platform-bus unsupported, force ID-mapping */
#if IS_ENABLED(CONFIG_IOMMU_API) && defined(CONFIG_INTEL_IOMMU)
#if KERNEL_VERSION(5, 9, 0) <= LINUX_VERSION_CODE
	memset(&iommu, 0, sizeof(iommu));
	iommu.priv = (void *)-1;
	pdev->dev.iommu = &iommu;
#else
#define INTEL_IOMMU_DUMMY_DOMAIN                ((void *)-1)
	pdev->dev.archdata.iommu = INTEL_IOMMU_DUMMY_DOMAIN;
#endif
#endif

	dev = drm_dev_alloc(&driver, &pdev->dev);
	if (IS_ERR(dev))
		return PTR_ERR(dev);

	ret = evdi_driver_setup(dev);
	if (ret)
		goto err_free;

	ret = drm_dev_register(dev, 0);
	if (ret)
		goto err_free;

	return 0;

err_free:
	drm_dev_put(dev);
	return ret;
}

static int evdi_platform_remove(struct platform_device *pdev)
{
	struct drm_device *drm_dev =
	    (struct drm_device *)platform_get_drvdata(pdev);
	EVDI_CHECKPT();

	drm_dev_unplug(drm_dev);

	return 0;
}

static void evdi_remove_all(void)
{
	int i;

	EVDI_DEBUG("removing all evdi devices\n");
	for (i = 0; i < evdi_context.dev_count; ++i) {
		if (evdi_context.devices[i]) {
			EVDI_DEBUG("removing evdi %d\n", i);

			platform_device_unregister(evdi_context.devices[i]);
			evdi_context.devices[i] = NULL;
		}
	}
	evdi_context.dev_count = 0;
}

static struct platform_driver evdi_platform_driver = {
	.probe = evdi_platform_probe,
	.remove = evdi_platform_remove,
	.driver = {
		   .name = "evdi",
		   .mod_name = KBUILD_MODNAME,
		   .owner = THIS_MODULE,
	}
};

static ssize_t version_show(__always_unused struct device *dev,
			    __always_unused struct device_attribute *attr,
			    char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u.%u.%u\n", DRIVER_MAJOR,
			DRIVER_MINOR, DRIVER_PATCH);
}

static ssize_t count_show(__always_unused struct device *dev,
			  __always_unused struct device_attribute *attr,
			  char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", evdi_context.dev_count);
}

static ssize_t add_store(__always_unused struct device *dev,
			 __always_unused struct device_attribute *attr,
			 const char *buf, size_t count)
{
	unsigned int val;
	int ret;

	if (kstrtouint(buf, 10, &val)) {
		EVDI_ERROR("Invalid device count \"%s\"\n", buf);
		return -EINVAL;
	}

	ret = evdi_add_devices(val);
	if (ret)
		return ret;

	return count;
}

static ssize_t remove_all_store(__always_unused struct device *dev,
				__always_unused struct device_attribute *attr,
				__always_unused const char *buf,
				size_t count)
{
	evdi_remove_all();
	return count;
}

static ssize_t loglevel_show(__always_unused struct device *dev,
			     __always_unused struct device_attribute *attr,
			     char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", evdi_loglevel);
}

static ssize_t loglevel_store(__always_unused struct device *dev,
			      __always_unused struct device_attribute *attr,
			      const char *buf,
			      size_t count)
{
	unsigned int val;

	if (kstrtouint(buf, 10, &val)) {
		EVDI_ERROR("Unable to parse %u\n", val);
		return -EINVAL;
	}
	if (val > EVDI_LOGLEVEL_VERBOSE) {
		EVDI_ERROR("Invalid loglevel %u\n", val);
		return -EINVAL;
	}

	EVDI_INFO("Setting loglevel to %u\n", val);
	evdi_loglevel = val;
	return count;
}

static struct device_attribute evdi_device_attributes[] = {
	__ATTR_RO(count),
	__ATTR_RO(version),
	__ATTR_RW(loglevel),
	__ATTR_WO(add),
	__ATTR_WO(remove_all)
};

static int __init evdi_init(void)
{
	int i, ret;

	EVDI_INFO("Initialising logging on level %u\n", evdi_loglevel);
	EVDI_INFO("Atomic driver:%s",
		(driver.driver_features & DRIVER_ATOMIC) ? "yes" : "no");

	evdi_context.root_dev = root_device_register("evdi");

	if (!PTR_ERR_OR_ZERO(evdi_context.root_dev))
		for (i = 0; i < ARRAY_SIZE(evdi_device_attributes); i++) {
			device_create_file(evdi_context.root_dev,
					   &evdi_device_attributes[i]);
		}

	ret = platform_driver_register(&evdi_platform_driver);
	if (ret)
		return ret;

	if (evdi_initial_device_count)
		return evdi_add_devices(evdi_initial_device_count);

	return 0;
}

static void __exit evdi_exit(void)
{
	int i;

	EVDI_CHECKPT();
	evdi_remove_all();
	platform_driver_unregister(&evdi_platform_driver);

	if (!PTR_ERR_OR_ZERO(evdi_context.root_dev)) {
		for (i = 0; i < ARRAY_SIZE(evdi_device_attributes); i++) {
			device_remove_file(evdi_context.root_dev,
					   &evdi_device_attributes[i]);
		}
		root_device_unregister(evdi_context.root_dev);
	}
}

module_init(evdi_init);
module_exit(evdi_exit);
