#pragma once
#include <sl.h>
#include <sl_dlss_g.h>


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

	private:

	};
}
