/*
** libhardware stub for loading a replacement Vulkan driver (e.g. Mesa/Turnip).
**
** A Turnip build is an Android Vulkan HAL and links against libhardware.so for
** hw_get_module(). That is a private platform library: it exists on the device at
** /system/lib64/libhardware.so, but an app's linker namespace
** ("classloader-namespace") only permits the public NDK libraries plus whatever ships
** in the APK, so dlopen of the driver fails with:
**
**   library "libhardware.so" not found: needed by .../vkdriver.so
**       in namespace classloader-namespace
**
** Shipping a stub under the same soname satisfies the dependency, because the app's
** own lib directory is searched inside the namespace.
**
** hw_get_module returning failure is the honest answer here: Turnip calls it to find
** the gralloc HAL for AHardwareBuffer interop, which the engine never uses - it
** renders to a plain swapchain. If a driver ever needs a real module this stub is
** where that shows up, as a clean -ENOENT rather than a crash.
*/

#include <errno.h>

struct hw_module_t;

int hw_get_module(const char *id, const struct hw_module_t **module)
{
	(void)id;
	if (module) *module = 0;
	return -ENOENT;
}

int hw_get_module_by_class(const char *class_id, const char *inst,
	const struct hw_module_t **module)
{
	(void)class_id;
	(void)inst;
	if (module) *module = 0;
	return -ENOENT;
}
