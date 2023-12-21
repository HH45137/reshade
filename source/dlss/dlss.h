#pragma once
#include <sl.h>
#include <sl_dlss_g.h>
#include <wrl/client.h>
#include <dxgi.h>


namespace DLSS {

	class DLSS3
	{
	public:
		DLSS3() = default;
		~DLSS3() = default;

		static void init()
		{
			sl::Preferences pref;
			pref.showConsole = true;
			pref.pathsToPlugins = {}; // change this if Streamline plugins are not located next to the executable
			pref.numPathsToPlugins = 0; // change this if Streamline plugins are not located next to the executable
			pref.pathToLogsAndData = {}; // change this to enable logging to a file
			pref.logMessageCallback = nullptr; // highly recommended to track warning/error messages in your callback
			pref.applicationId = 1; // Provided by NVDA, required if using NGX components (DLSS 2/3)
			pref.engine = sl::EngineType::eCustom;
			pref.engineVersion = "1.0"; // Optional version
			pref.projectId = "1.0"; // Optional project id
			if (SL_FAILED(res, slInit(pref)))
			{
				// Handle error, check the logs
				if (res == sl::Result::eErrorDriverOutOfDate) { /* inform user */ }
				// and so on ...
			}
		}

		static int select_device(void *d3dDevice) {

			if (SL_FAILED(res, slSetD3DDevice(d3dDevice))) {
				return -1;
			}

			return 0;
		}

		static bool is_support_fg() {

			Microsoft::WRL::ComPtr<IDXGIFactory> factory;
			if (SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void **)&factory)))
			{
				Microsoft::WRL::ComPtr<IDXGIAdapter> adapter {};
				uint32_t i = 0;
				while (factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND)
				{
					DXGI_ADAPTER_DESC desc {};
					if (SUCCEEDED(adapter->GetDesc(&desc)))
					{
						sl::AdapterInfo adapterInfo {};
						adapterInfo.deviceLUID = (uint8_t *)&desc.AdapterLuid;
						adapterInfo.deviceLUIDSizeInBytes = sizeof(LUID);
						if (SL_FAILED(result, slIsFeatureSupported(sl::kFeatureDLSS_G, adapterInfo)))
						{
							// Requested feature is not supported on the system, fallback to the default method
							switch (result)
							{
							case sl::Result::eErrorOSOutOfDate:         // inform user to update OS
								return false;
							case sl::Result::eErrorDriverOutOfDate:     // inform user to update driver
								return false;
							case sl::Result::eErrorNoSupportedAdapterFound:  // cannot use this adapter (older or non-NVDA GPU etc)
								return false;
								// and so on ...
							};

							return false;
						}
						else
						{
							// Feature is supported on this adapter!
							return true;
						}
					}
					i++;
				}
			}

			return false;
		}

	private:

	};
}
