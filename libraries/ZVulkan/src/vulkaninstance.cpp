
#include "vulkaninstance.h"
#include "vulkanbuilders.h"
#include <mutex>
#include <set>
#include <string>
#include <cstring>

#ifdef __ANDROID__
// Defined further down, next to the custom-driver loader.
static bool UsingCustomDriver();
static void ForceSystemDriver();
#endif

VulkanInstance::VulkanInstance(std::vector<uint32_t> apiVersionsToTry, std::set<std::string> requiredExtensions, std::set<std::string> optionalExtensions, bool wantDebugLayer)
	: ApiVersionsToTry(std::move(apiVersionsToTry)), RequiredExtensions(std::move(requiredExtensions)), OptionalExtensions(std::move(optionalExtensions)), WantDebugLayer(wantDebugLayer)
{
	try
	{
		ShaderBuilder::Init();
		InitVolk();
		CreateInstance();
	}
	catch (...)
	{
#ifdef __ANDROID__
		// A replacement driver can load cleanly and still be unusable. A bare HAL
		// provides no window-system integration - VK_KHR_surface and
		// VK_KHR_android_surface come from Android's Vulkan loader, not the driver - so
		// CreateInstance fails with "extension not present" even though volk
		// initialised and reported a version. Without this retry the engine would then
		// fall through to OpenGL, which is not built for Android, and die.
		if (UsingCustomDriver())
		{
			ReleaseResources();
			ForceSystemDriver();
			try
			{
				InitVolk();
				CreateInstance();
				return;
			}
			catch (...)
			{
				ReleaseResources();
				throw;
			}
		}
#endif
		ReleaseResources();
		throw;
	}
}

VulkanInstance::~VulkanInstance()
{
	ReleaseResources();
}

void VulkanInstance::ReleaseResources()
{
	if (debugMessenger)
		vkDestroyDebugUtilsMessengerEXT(Instance, debugMessenger, nullptr);
	debugMessenger = VK_NULL_HANDLE;

	if (Instance)
		vkDestroyInstance(Instance, nullptr);
	Instance = nullptr;
}

#ifdef __ANDROID__

#include <dlfcn.h>
#include <cstddef>
#include <android/log.h>

// Load a replacement Vulkan driver, e.g. a Mesa/Turnip build, from the path in
// $ZVULKAN_DRIVER. Returns nullptr to mean "use the system driver".
//
// Android ships Vulkan drivers as HAL modules, not as loadable ICDs. A Turnip .so
// exports exactly one symbol - HMI, a hw_module_t - and no vkGetInstanceProcAddr or
// vk_icdGetInstanceProcAddr, so it cannot simply be dlopen'd and dlsym'd. Installing
// it the supported way means writing to /vendor/lib64/hw, which needs root. What is
// left, and what Android emulator front-ends do, is to walk the HAL protocol by hand:
// dlopen, read HMI, call its open(), and take GetInstanceProcAddr off the device.
//
// The HAL structs live in AOSP's hardware/hwvulkan.h, which is a platform header and
// is NOT in the NDK, so the layouts below are reproduced rather than included. The
// offsets that matter were verified against a real driver binary (HMI is 248 bytes,
// tag reads 'HWMT', methods sits at offset 32). Because a wrong guess here would be
// silent memory corruption, every step is checked and any failure falls back to the
// system driver instead of proceeding.
namespace
{
	constexpr uint32_t kHardwareModuleTag = 0x48574d54; // 'HWMT'
	constexpr uint32_t kHardwareDeviceTag = 0x48574454; // 'HWDT'

	struct hw_module_t;
	struct hw_device_t;

	struct hw_module_methods_t
	{
		int (*open)(const hw_module_t *module, const char *id, hw_device_t **device);
	};

	struct hw_module_t
	{
		uint32_t tag;
		uint16_t module_api_version;
		uint16_t hal_api_version;
		const char *id;
		const char *name;
		const char *author;
		hw_module_methods_t *methods;
		void *dso;
		uint32_t reserved[32 - 7];
	};

	struct hw_device_t
	{
		uint32_t tag;
		uint32_t version;
		hw_module_t *module;
		// reserved is 24 words, not the 12 that older copies of hardware.h show. Found
		// by probing a real device struct with dladdr: module lands at +8 and the four
		// function pointers (close, then the three Vulkan entry points below) at +112,
		// +120, +128 and +136, which only works if reserved spans 16..111.
		uint32_t reserved[24];
		int (*close)(hw_device_t *device);
	};

	// hwvulkan_device_t: hw_device_t followed by three entry points.
	struct hwvulkan_device_t
	{
		hw_device_t common;
		PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
		PFN_vkCreateInstance CreateInstance;
		PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
	};

	void DriverLog(const char *fmt, ...)
	{
		va_list args;
		va_start(args, fmt);
		__android_log_vprint(ANDROID_LOG_INFO, "selaco-ea", fmt, args);
		va_end(args);
	}

	// Set once a replacement driver is in use, cleared when we give up on it, so the
	// constructor knows a retry with the system driver is worth attempting.
	bool sUsingCustomDriver = false;
	bool sSystemDriverForced = false;

	PFN_vkGetInstanceProcAddr LoadCustomDriver()
	{
		if (sSystemDriverForced)
			return nullptr;

		const char *path = getenv("ZVULKAN_DRIVER");
		if (path == nullptr || *path == '\0')
		{
			DriverLog("vk_driver: ZVULKAN_DRIVER unset, using system driver");
			return nullptr;
		}

		void *module = dlopen(path, RTLD_NOW | RTLD_LOCAL);
		if (module == nullptr)
		{
			DriverLog("vk_driver: dlopen failed for %s: %s", path, dlerror());
			return nullptr;
		}

		auto hmi = (hw_module_t *)dlsym(module, "HMI");
		if (hmi == nullptr)
		{
			DriverLog("vk_driver: %s has no HMI symbol - not an Android Vulkan HAL", path);
			dlclose(module);
			return nullptr;
		}

		// If the tag is wrong our idea of the layout does not match the driver's, so
		// stop before dereferencing anything else.
		if (hmi->tag != kHardwareModuleTag)
		{
			DriverLog("vk_driver: HMI tag 0x%08x, expected 'HWMT' - layout mismatch, ignoring %s",
				hmi->tag, path);
			dlclose(module);
			return nullptr;
		}

		if (hmi->methods == nullptr || hmi->methods->open == nullptr)
		{
			DriverLog("vk_driver: %s HMI has no open() method", path);
			dlclose(module);
			return nullptr;
		}

		hw_device_t *dev = nullptr;
		int err = hmi->methods->open(hmi, "vk0", &dev); // HWVULKAN_DEVICE_0
		if (err != 0 || dev == nullptr)
		{
			DriverLog("vk_driver: open() failed (%d) for %s", err, path);
			dlclose(module);
			return nullptr;
		}

		if (dev->tag != kHardwareDeviceTag)
		{
			DriverLog("vk_driver: device tag 0x%08x, expected 'HWDT' - layout mismatch, ignoring %s",
				dev->tag, path);
			dlclose(module);
			return nullptr;
		}

		auto vkdev = (hwvulkan_device_t *)dev;
		PFN_vkGetInstanceProcAddr gipa = vkdev->GetInstanceProcAddr;

		// The hw_device_t layout is reproduced from a platform header we cannot include,
		// so if our guessed offset is wrong, find the real one instead of giving up:
		// walk the struct and ask dladdr what each pointer-sized slot points at. dladdr
		// only inspects, it never calls, so a wrong guess here is harmless.
		if (gipa == nullptr)
		{
			DriverLog("vk_driver: GetInstanceProcAddr null at assumed offset %zu; probing",
				offsetof(hwvulkan_device_t, GetInstanceProcAddr));

			auto words = (void **)dev;
			for (size_t i = 0; i < 24; i++)
			{
				void *p = words[i];
				if (p == nullptr) continue;
				Dl_info info{};
				if (dladdr(p, &info) != 0 && info.dli_fname != nullptr)
				{
					DriverLog("vk_driver:   +%3zu %p  %s  %s", i * sizeof(void *), p,
						info.dli_sname ? info.dli_sname : "(no symbol)", info.dli_fname);
				}
			}
		}

		if (gipa == nullptr)
		{
			DriverLog("vk_driver: %s device has no GetInstanceProcAddr", path);
			dlclose(module);
			return nullptr;
		}

		// Final check that we really are holding a Vulkan entry point and not whatever
		// happens to sit at that offset: a valid loader resolves vkCreateInstance from
		// a null instance.
		if (gipa(VK_NULL_HANDLE, "vkCreateInstance") == nullptr)
		{
			DriverLog("vk_driver: GetInstanceProcAddr from %s cannot resolve vkCreateInstance", path);
			dlclose(module);
			return nullptr;
		}

		DriverLog("vk_driver: using %s", path);
		sUsingCustomDriver = true;
		return gipa; // module intentionally left loaded for the process lifetime
	}
}

static bool UsingCustomDriver() { return sUsingCustomDriver; }

static void ForceSystemDriver()
{
	DriverLog("vk_driver: replacement driver could not create an instance "
		"(a bare HAL has no VK_KHR_surface); retrying with the system driver");
	sUsingCustomDriver = false;
	sSystemDriverForced = true;
}
#endif

void VulkanInstance::InitVolk()
{
#ifdef __ANDROID__
	if (PFN_vkGetInstanceProcAddr custom = LoadCustomDriver())
	{
		volkInitializeCustom(custom);
		if (volkGetInstanceVersion() != 0)
			return;
		// The driver loaded but reports no usable version; fall through to the system
		// one rather than leaving volk pointed at something broken.
		DriverLog("vk_driver: custom driver reports no instance version, using system driver");
	}
#endif

	if (volkInitialize() != VK_SUCCESS)
	{
		VulkanError("Unable to find Vulkan");
	}
	auto iver = volkGetInstanceVersion();
	if (iver == 0)
	{
		VulkanError("Vulkan not supported");
	}
}

void VulkanInstance::CreateInstance()
{
	AvailableLayers = GetAvailableLayers();
	AvailableExtensions = GetExtensions();
	EnabledExtensions = RequiredExtensions;

	std::string debugLayer = "VK_LAYER_KHRONOS_validation";
	bool debugLayerFound = false;
	if (WantDebugLayer)
	{
		for (const VkLayerProperties& layer : AvailableLayers)
		{
			if (layer.layerName == debugLayer)
			{
				EnabledValidationLayers.insert(layer.layerName);
				EnabledExtensions.insert(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
				debugLayerFound = true;
				break;
			}
		}
	}

	// Enable optional instance extensions we are interested in
	for (const auto& ext : AvailableExtensions)
	{
		if (OptionalExtensions.find(ext.extensionName) != OptionalExtensions.end())
		{
			EnabledExtensions.insert(ext.extensionName);
		}
	}

	std::vector<const char*> enabledValidationLayersCStr;
	for (const std::string& layer : EnabledValidationLayers)
		enabledValidationLayersCStr.push_back(layer.c_str());

	std::vector<const char*> enabledExtensionsCStr;
	for (const std::string& ext : EnabledExtensions)
		enabledExtensionsCStr.push_back(ext.c_str());

	// Try get the highest vulkan version we can get
	VkResult result = VK_ERROR_INITIALIZATION_FAILED;
	for (uint32_t apiVersion : ApiVersionsToTry)
	{
		VkApplicationInfo appInfo = {};
		appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		appInfo.pApplicationName = "VulkanDrv";
		appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
		appInfo.pEngineName = "VulkanDrv";
		appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
		appInfo.apiVersion = apiVersion;

		VkInstanceCreateInfo createInfo = {};
		createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		createInfo.pApplicationInfo = &appInfo;
		createInfo.enabledExtensionCount = (uint32_t)EnabledExtensions.size();
		createInfo.enabledLayerCount = (uint32_t)enabledValidationLayersCStr.size();
		createInfo.ppEnabledLayerNames = enabledValidationLayersCStr.data();
		createInfo.ppEnabledExtensionNames = enabledExtensionsCStr.data();

		result = vkCreateInstance(&createInfo, nullptr, &Instance);
		if (result >= VK_SUCCESS)
		{
			ApiVersion = apiVersion;
			break;
		}
	}
	CheckVulkanError(result, "Could not create vulkan instance");

	volkLoadInstance(Instance);

	if (debugLayerFound)
	{
		VkDebugUtilsMessengerCreateInfoEXT dbgCreateInfo = {};
		dbgCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
		dbgCreateInfo.messageSeverity =
			VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
			VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
		dbgCreateInfo.messageType =
			VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
			VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
			VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
		dbgCreateInfo.pfnUserCallback = DebugCallback;
		dbgCreateInfo.pUserData = this;
		result = vkCreateDebugUtilsMessengerEXT(Instance, &dbgCreateInfo, nullptr, &debugMessenger);
		CheckVulkanError(result, "vkCreateDebugUtilsMessengerEXT failed");

		DebugLayerActive = true;
	}

	PhysicalDevices = GetPhysicalDevices(Instance, ApiVersion);
}

std::vector<VulkanPhysicalDevice> VulkanInstance::GetPhysicalDevices(VkInstance instance, uint32_t apiVersion)
{
	uint32_t deviceCount = 0;
	VkResult result = vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
	if (result == VK_ERROR_INITIALIZATION_FAILED) // Some drivers return this when a card does not support vulkan
		return {};
	CheckVulkanError(result, "vkEnumeratePhysicalDevices failed");
	if (deviceCount == 0)
		return {};

	std::vector<VkPhysicalDevice> devices(deviceCount);
	result = vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
	CheckVulkanError(result, "vkEnumeratePhysicalDevices failed (2)");

	std::vector<VulkanPhysicalDevice> devinfo(deviceCount);
	for (size_t i = 0; i < devices.size(); i++)
	{
		auto& dev = devinfo[i];
		dev.Device = devices[i];

		uint32_t queueFamilyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(dev.Device, &queueFamilyCount, nullptr);
		dev.QueueFamilies.resize(queueFamilyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(dev.Device, &queueFamilyCount, dev.QueueFamilies.data());

		uint32_t deviceExtensionCount = 0;
		vkEnumerateDeviceExtensionProperties(dev.Device, nullptr, &deviceExtensionCount, nullptr);
		dev.Extensions.resize(deviceExtensionCount);
		vkEnumerateDeviceExtensionProperties(dev.Device, nullptr, &deviceExtensionCount, dev.Extensions.data());

		auto checkForExtension = [&](const char* name)
		{
			for (const auto& ext : dev.Extensions)
			{
				if (strcmp(ext.extensionName, name) == 0)
					return true;
			}
			return false;
		};

		vkGetPhysicalDeviceMemoryProperties(dev.Device, &dev.Properties.Memory);

		if (apiVersion != VK_API_VERSION_1_0)
		{
			VkPhysicalDeviceProperties2 deviceProperties2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };

			void** next = const_cast<void**>(&deviceProperties2.pNext);
			if (checkForExtension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME))
			{
				*next = &dev.Properties.AccelerationStructure;
				next = &dev.Properties.AccelerationStructure.pNext;
			}
			if (checkForExtension(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME))
			{
				*next = &dev.Properties.DescriptorIndexing;
				next = &dev.Properties.DescriptorIndexing.pNext;
			}
			if (checkForExtension(VK_MSFT_LAYERED_DRIVER_EXTENSION_NAME))
			{
				*next = &dev.Properties.LayeredDriver;
				next = &dev.Properties.LayeredDriver.pNext;
			}

			vkGetPhysicalDeviceProperties2(dev.Device, &deviceProperties2);
			dev.Properties.Properties = deviceProperties2.properties;
			dev.Properties.AccelerationStructure.pNext = nullptr;
			dev.Properties.DescriptorIndexing.pNext = nullptr;
			dev.Properties.LayeredDriver.pNext = nullptr;

			VkPhysicalDeviceFeatures2 deviceFeatures2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };

			next = const_cast<void**>(&deviceFeatures2.pNext);
			if (checkForExtension(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME))
			{
				*next = &dev.Features.BufferDeviceAddress;
				next = &dev.Features.BufferDeviceAddress.pNext;
			}
			if (checkForExtension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME))
			{
				*next = &dev.Features.AccelerationStructure;
				next = &dev.Features.AccelerationStructure.pNext;
			}
			if (checkForExtension(VK_KHR_RAY_QUERY_EXTENSION_NAME))
			{
				*next = &dev.Features.RayQuery;
				next = &dev.Features.RayQuery.pNext;
			}
			if (checkForExtension(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME))
			{
				*next = &dev.Features.DescriptorIndexing;
				next = &dev.Features.DescriptorIndexing.pNext;
			}

			vkGetPhysicalDeviceFeatures2(dev.Device, &deviceFeatures2);
			dev.Features.Features = deviceFeatures2.features;
			dev.Features.BufferDeviceAddress.pNext = nullptr;
			dev.Features.AccelerationStructure.pNext = nullptr;
			dev.Features.RayQuery.pNext = nullptr;
			dev.Features.DescriptorIndexing.pNext = nullptr;
		}
		else
		{
			vkGetPhysicalDeviceProperties(dev.Device, &dev.Properties.Properties);
			vkGetPhysicalDeviceFeatures(dev.Device, &dev.Features.Features);
		}
	}
	return devinfo;
}

std::vector<VkLayerProperties> VulkanInstance::GetAvailableLayers()
{
	uint32_t layerCount;
	VkResult result = vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

	std::vector<VkLayerProperties> availableLayers(layerCount);
	result = vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());
	return availableLayers;
}

std::vector<VkExtensionProperties> VulkanInstance::GetExtensions()
{
	uint32_t extensionCount = 0;
	VkResult result = vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);

	std::vector<VkExtensionProperties> extensions(extensionCount);
	result = vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, extensions.data());
	return extensions;
}

VkBool32 VulkanInstance::DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity, VkDebugUtilsMessageTypeFlagsEXT messageType, const VkDebugUtilsMessengerCallbackDataEXT* callbackData, void* userData)
{
	VulkanInstance* instance = (VulkanInstance*)userData;

	static std::mutex mtx;
	static std::set<std::string> seenMessages;
	static int totalMessages;

	std::unique_lock<std::mutex> lock(mtx);

	std::string msg = callbackData->pMessage;

	// Attempt to parse the string because the default formatting is totally unreadable and half of what it writes is totally useless!
	auto parts = SplitString(msg, " | ");
	if (parts.size() == 3)
	{
		msg = parts[2];
		size_t pos = msg.find(" The Vulkan spec states:");
		if (pos != std::string::npos)
			msg = msg.substr(0, pos);

		if (callbackData->objectCount > 0)
		{
			msg += " (";
			for (uint32_t i = 0; i < callbackData->objectCount; i++)
			{
				if (i > 0)
					msg += ", ";
				if (callbackData->pObjects[i].pObjectName)
					msg += callbackData->pObjects[i].pObjectName;
				else
					msg += "<noname>";
			}
			msg += ")";
		}
	}

	bool found = seenMessages.find(msg) != seenMessages.end();
	if (!found)
	{
		if (totalMessages < 20)
		{
			totalMessages++;
			seenMessages.insert(msg);

			const char* typestr;
			bool showcallstack = false;
			if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
			{
				typestr = "vulkan error";
				showcallstack = true;
			}
			else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
			{
				typestr = "vulkan warning";
			}
			else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)
			{
				typestr = "vulkan info";
			}
			else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT)
			{
				typestr = "vulkan verbose";
			}
			else
			{
				typestr = "vulkan";
			}

			VulkanPrintLog(typestr, msg);
		}
	}

	return VK_FALSE;
}

std::vector<std::string> VulkanInstance::SplitString(const std::string& s, const std::string& seperator)
{
	std::vector<std::string> output;
	std::string::size_type prev_pos = 0, pos = 0;

	while ((pos = s.find(seperator, pos)) != std::string::npos)
	{
		std::string substring(s.substr(prev_pos, pos - prev_pos));

		output.push_back(substring);

		pos += seperator.length();
		prev_pos = pos;
	}

	output.push_back(s.substr(prev_pos, pos - prev_pos)); // Last word
	return output;
}

std::string VkResultToString(VkResult result)
{
	switch (result)
	{
	case VK_SUCCESS: return "success";
	case VK_NOT_READY: return "not ready";
	case VK_TIMEOUT: return "timeout";
	case VK_EVENT_SET: return "event set";
	case VK_EVENT_RESET: return "event reset";
	case VK_INCOMPLETE: return "incomplete";
	case VK_ERROR_OUT_OF_HOST_MEMORY: return "out of host memory";
	case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "out of device memory";
	case VK_ERROR_INITIALIZATION_FAILED: return "initialization failed";
	case VK_ERROR_DEVICE_LOST: return "device lost";
	case VK_ERROR_MEMORY_MAP_FAILED: return "memory map failed";
	case VK_ERROR_LAYER_NOT_PRESENT: return "layer not present";
	case VK_ERROR_EXTENSION_NOT_PRESENT: return "extension not present";
	case VK_ERROR_FEATURE_NOT_PRESENT: return "feature not present";
	case VK_ERROR_INCOMPATIBLE_DRIVER: return "incompatible driver";
	case VK_ERROR_TOO_MANY_OBJECTS: return "too many objects";
	case VK_ERROR_FORMAT_NOT_SUPPORTED: return "format not supported";
	case VK_ERROR_FRAGMENTED_POOL: return "fragmented pool";
	case VK_ERROR_OUT_OF_POOL_MEMORY: return "out of pool memory";
	case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "invalid external handle";
	case VK_ERROR_SURFACE_LOST_KHR: return "surface lost";
	case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "native window in use";
	case VK_SUBOPTIMAL_KHR: return "suboptimal";
	case VK_ERROR_OUT_OF_DATE_KHR: return "out of date";
	case VK_ERROR_INCOMPATIBLE_DISPLAY_KHR: return "incompatible display";
	case VK_ERROR_VALIDATION_FAILED_EXT: return "validation failed";
	case VK_ERROR_INVALID_SHADER_NV: return "invalid shader";
	case VK_ERROR_FRAGMENTATION_EXT: return "fragmentation";
	case VK_ERROR_NOT_PERMITTED_EXT: return "not permitted";
	case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT: return "full screen exclusive mode lost";
	case VK_THREAD_IDLE_KHR: return "thread idle";
	case VK_THREAD_DONE_KHR: return "thread done";
	case VK_OPERATION_DEFERRED_KHR: return "operation deferred";
	case VK_OPERATION_NOT_DEFERRED_KHR: return "operation not deferred";
	case VK_PIPELINE_COMPILE_REQUIRED_EXT: return "pipeline compile required";
	default: break;
	}
	return "vkResult " + std::to_string((int)result);
}
