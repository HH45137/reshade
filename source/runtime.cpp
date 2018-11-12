/**
 * Copyright (C) 2014 Patrick Mours. All rights reserved.
 * License: https://github.com/crosire/reshade#license
 */

#include "log.hpp"
#include "version.h"
#include "runtime.hpp"
#include "effect_parser.hpp"
#include "effect_preprocessor.hpp"
#include "input.hpp"
#include "ini_file.hpp"
#include <assert.h>
#include <thread>
#include <algorithm>
#include <unordered_set>
#include <stb_image.h>
#include <stb_image_dds.h>
#include <stb_image_write.h>
#include <stb_image_resize.h>

reshade::runtime::runtime(uint32_t renderer) :
	_renderer_id(renderer),
	_start_time(std::chrono::high_resolution_clock::now()),
	_last_present_time(std::chrono::high_resolution_clock::now()),
	_last_frame_duration(std::chrono::milliseconds(1)),
	_effect_search_paths({ g_reshade_dll_path.parent_path() }),
	_texture_search_paths({ g_reshade_dll_path.parent_path() }),
	_preprocessor_definitions({
		"RESHADE_DEPTH_LINEARIZATION_FAR_PLANE=1000.0",
		"RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN=0",
		"RESHADE_DEPTH_INPUT_IS_REVERSED=1",
		"RESHADE_DEPTH_INPUT_IS_LOGARITHMIC=0" }),
	_menu_key_data(),
	_screenshot_key_data(),
	_reload_key_data(),
	_effects_key_data(),
	_screenshot_path(g_target_executable_path.parent_path()),
	_variable_editor_height(500)
{
	_screenshot_key_data[0] = 0x2C; // VK_SNAPSHOT

	_configuration_path = g_reshade_dll_path;
	_configuration_path.replace_extension(".ini");
	if (std::error_code ec; !std::filesystem::exists(_configuration_path, ec))
		_configuration_path = g_reshade_dll_path.parent_path() / "ReShade.ini";

	_needs_update = check_for_update(_latest_version);

	init_ui();
	load_config();
}
reshade::runtime::~runtime()
{
	deinit_ui();

	assert(!_is_initialized && _techniques.empty());
}

bool reshade::runtime::on_init()
{
	LOG(INFO) << "Recreated runtime environment on runtime " << this << ".";

	_is_initialized = true;
	_last_reload_time = std::chrono::high_resolution_clock::now();

	if (!_no_reload_on_init)
		reload();

	return true;
}
void reshade::runtime::on_reset()
{
	on_reset_effect();

	if (!_is_initialized)
		return;

	_imgui_font_atlas_texture.reset();

	LOG(INFO) << "Destroyed runtime environment on runtime " << this << ".";

	_width = _height = 0;
	_is_initialized = false;
}
void reshade::runtime::on_reset_effect()
{
	_textures.clear();
	_uniforms.clear();
	_techniques.clear();
	_uniform_data_storage.clear();
}
void reshade::runtime::on_present()
{
	// Get current time and date
	time_t t = std::time(nullptr); tm tm;
	localtime_s(&tm, &t);
	_date[0] = tm.tm_year + 1900;
	_date[1] = tm.tm_mon + 1;
	_date[2] = tm.tm_mday;
	_date[3] = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;

	// Advance various statistics
	_framecount++;
	_last_present_time += _last_frame_duration = std::chrono::high_resolution_clock::now() - _last_present_time;

	if (_input->is_key_pressed(_reload_key_data))
		reload();

	// Create and save screenshot if associated shortcut is down
	if (!_screenshot_key_setting_active && _input->is_key_pressed(_screenshot_key_data))
		save_screenshot();

	if (_has_finished_reloading)
	{
		load_current_preset();
		load_textures();

		_has_finished_reloading = false;
	}
	else if (!_reload_queue.empty())
	{
		effect_data &effect = _loaded_effects[_reload_queue.back()];

		_reload_queue.pop_back();

		if (!compile_effect(effect))
		{
			LOG(ERROR) << "Failed to compile " << effect.source_file << ":\n" << effect.errors;
			_last_reload_successful = false;
		}
	}

	// Draw overlay
	draw_ui();

	// Reset input status
	_input->next_frame();

	g_network_traffic = _drawcalls = _vertices = 0;
}
void reshade::runtime::on_present_effect()
{
	if (!_toggle_key_setting_active && _input->is_key_pressed(_effects_key_data))
		_effects_enabled = !_effects_enabled;
	if (!_effects_enabled) // Nothing to do here if effects are disabled globally
		return;

	// Update special uniform variables
	for (uniform &variable : _uniforms)
	{
		switch (variable.special)
		{
		case special_uniform::frame_time:
			set_uniform_value(variable, _last_frame_duration.count() * 1e-6f, 0.0f, 0.0f, 0.0f);
			break;
		case special_uniform::frame_count:
			if (variable.type.is_boolean())
				set_uniform_value(variable, (_framecount % 2) == 0);
			else
				set_uniform_value(variable, static_cast<unsigned int>(_framecount % UINT_MAX));
			break;
		case special_uniform::random: {
			const int min = variable.annotations["min"].second.as_int[0];
			const int max = variable.annotations["max"].second.as_int[0];
			set_uniform_value(variable, min + (std::rand() % (max - min + 1)));
			break; }
		case special_uniform::ping_pong: {
			const float min = variable.annotations["min"].second.as_float[0];
			const float max = variable.annotations["max"].second.as_float[0];
			const float step_min = variable.annotations["step"].second.as_float[0];
			const float step_max = variable.annotations["step"].second.as_float[1];
			float increment = step_max == 0 ? step_min : (step_min + std::fmodf(static_cast<float>(std::rand()), step_max - step_min + 1));
			const float smoothing = variable.annotations["smoothing"].second.as_float[0];

			float value[2] = { 0, 0 };
			get_uniform_value(variable, value, 2);
			if (value[1] >= 0)
			{
				increment = std::max(increment - std::max(0.0f, smoothing - (max - value[0])), 0.05f);
				increment *= _last_frame_duration.count() * 1e-9f;

				if ((value[0] += increment) >= max)
					value[0] = max, value[1] = -1;
			}
			else
			{
				increment = std::max(increment - std::max(0.0f, smoothing - (value[0] - min)), 0.05f);
				increment *= _last_frame_duration.count() * 1e-9f;

				if ((value[0] -= increment) <= min)
					value[0] = min, value[1] = +1;
			}
			set_uniform_value(variable, value, 2);
			break; }
		case special_uniform::date:
			set_uniform_value(variable, _date, 4);
			break;
		case special_uniform::timer:
			set_uniform_value(variable, std::chrono::duration_cast<std::chrono::nanoseconds>(_last_present_time - _start_time).count() * 1e-6f);
			break;
		case special_uniform::key:
			if (const int keycode = variable.annotations["keycode"].second.as_int[0];
				keycode > 7 && keycode < 256)
			{
				const auto &mode = variable.annotations["mode"].second.string_data;
				if (mode == "toggle" || variable.annotations["toggle"].second.as_uint[0])
				{
					bool current_value = false;
					get_uniform_value(variable, &current_value, 1);
					if (_input->is_key_pressed(keycode))
						set_uniform_value(variable, !current_value);
				}
				else if (mode == "press")
					set_uniform_value(variable, _input->is_key_pressed(keycode));
				else
					set_uniform_value(variable, _input->is_key_down(keycode));
			}
			break;
		case special_uniform::mouse_point:
			set_uniform_value(variable, _input->mouse_position_x(), _input->mouse_position_y());
			break;
		case special_uniform::mouse_delta:
			set_uniform_value(variable, _input->mouse_movement_delta_x(), _input->mouse_movement_delta_y());
			break;
		case special_uniform::mouse_button:
			if (const int keycode = variable.annotations["keycode"].second.as_int[0];
				keycode >= 0 && keycode < 5)
			{
				const auto &mode = variable.annotations["mode"].second.string_data;
				if (mode == "toggle" || variable.annotations["toggle"].second.as_uint[0])
				{
					bool current_value = false;
					get_uniform_value(variable, &current_value, 1);
					if (_input->is_mouse_button_pressed(keycode))
						set_uniform_value(variable, !current_value);
				}
				else if (mode == "press")
					set_uniform_value(variable, _input->is_mouse_button_pressed(keycode));
				else
					set_uniform_value(variable, _input->is_mouse_button_down(keycode));
			}
			break;
		}
	}

	// Render all enabled techniques
	for (technique &technique : _techniques)
	{
		if (technique.timeleft > 0)
		{
			technique.timeleft -= std::chrono::duration_cast<std::chrono::milliseconds>(_last_frame_duration).count();
			if (technique.timeleft <= 0)
				disable_technique(technique);
		}
		else if (!_toggle_key_setting_active && _input->is_key_pressed(technique.toggle_key_data)
			|| (technique.toggle_key_data[0] >= 0x01 && technique.toggle_key_data[0] <= 0x06 && _input->is_mouse_button_pressed(technique.toggle_key_data[0] - 1)))
		{
			if (!technique.enabled)
				enable_technique(technique);
			else
				disable_technique(technique);
		}

		if (technique.impl == nullptr || !technique.enabled)
			continue; // Ignore techniques that are not fully loaded or currently disabled

		const auto time_technique_started = std::chrono::high_resolution_clock::now();
		render_technique(technique);
		const auto time_technique_finished = std::chrono::high_resolution_clock::now();

		technique.average_cpu_duration.append(std::chrono::duration_cast<std::chrono::nanoseconds>(time_technique_finished - time_technique_started).count());
	}
}

void reshade::runtime::reload()
{
	on_reset_effect();

	_loaded_effects.clear();
	_last_reload_successful = true;
	_has_finished_reloading = false;

	std::vector<std::filesystem::path> effect_files;
	for (const auto &search_path : _effect_search_paths)
	{
		std::error_code ec;
		for (const auto &entry : std::filesystem::directory_iterator(search_path, ec))
			if (entry.path().extension() == ".fx")
				effect_files.push_back(entry.path());
	}

	_reload_remaining_effects = effect_files.size();

	for (const auto &file : effect_files)
		std::thread([this, file]() { load_effect(file); }).detach();
}
void reshade::runtime::load_effect(const std::filesystem::path &path)
{
	std::string source_code, errors;
	reshadefx::module module;

	{ reshadefx::preprocessor pp;

		if (path.is_absolute())
			pp.add_include_path(path.parent_path());

		for (const auto &include_path : _effect_search_paths)
		{
			if (include_path.empty())
				continue; // Skip invalid paths

			pp.add_include_path(include_path);
		}

		pp.add_macro_definition("__RESHADE__", std::to_string(VERSION_MAJOR * 10000 + VERSION_MINOR * 100 + VERSION_REVISION));
		pp.add_macro_definition("__RESHADE_PERFORMANCE_MODE__", _performance_mode ? "1" : "0");
		pp.add_macro_definition("__VENDOR__", std::to_string(_vendor_id));
		pp.add_macro_definition("__DEVICE__", std::to_string(_device_id));
		pp.add_macro_definition("__RENDERER__", std::to_string(_renderer_id));
		pp.add_macro_definition("__APPLICATION__", std::to_string(std::hash<std::string>()(g_target_executable_path.stem().u8string())));
		pp.add_macro_definition("BUFFER_WIDTH", std::to_string(_width));
		pp.add_macro_definition("BUFFER_HEIGHT", std::to_string(_height));
		pp.add_macro_definition("BUFFER_RCP_WIDTH", std::to_string(1.0f / static_cast<float>(_width)));
		pp.add_macro_definition("BUFFER_RCP_HEIGHT", std::to_string(1.0f / static_cast<float>(_height)));

		for (const auto &definition : _preprocessor_definitions)
		{
			if (definition.empty())
				continue; // Skip invalid definitions

			const size_t equals_index = definition.find_first_of('=');
			if (equals_index != std::string::npos)
				pp.add_macro_definition(definition.substr(0, equals_index), definition.substr(equals_index + 1));
			else
				pp.add_macro_definition(definition);
		}

		// Pre-process the source file
		if (!pp.append_file(path))
		{
			LOG(ERROR) << "Failed to pre-process " << path << ":\n" << pp.errors();

			_last_reload_successful = false;
			if (--_reload_remaining_effects == 0)
			{
				_last_reload_time = std::chrono::high_resolution_clock::now();
				_has_finished_reloading = true;
			}
			return;
		}

		errors = std::move(pp.errors()); // Append any pre-processor warnings to the error list
		source_code = std::move(pp.output());
	}

	{ reshadefx::parser parser;

		unsigned shader_model;
		if (_renderer_id < 0xa000)
			shader_model = 30;
		else if (_renderer_id < 0xa100)
			shader_model = 40;
		else if (_renderer_id < 0xb000)
			shader_model = 41;
		else
			shader_model = 50;

		// Compile the pre-processed source code
		if (!parser.parse(source_code, _renderer_id & 0x10000 ? reshadefx::codegen::backend::glsl : reshadefx::codegen::backend::hlsl, shader_model, true, _performance_mode, module))
		{
			LOG(ERROR) << "Failed to compile " << path << ":\n" << parser.errors();

			_last_reload_successful = false;
			if (--_reload_remaining_effects == 0)
			{
				_last_reload_time = std::chrono::high_resolution_clock::now();
				_has_finished_reloading = true;
			}
			return;
		}

		errors += parser.errors();
	}

#if RESHADE_DUMP_NATIVE_SHADERS
	std::ofstream("ReShade-ShaderDump-" + path.stem().u8string() + ".hlsl", std::ios::trunc) << module.hlsl;
#endif

	// Fill all specialization constants with values from the current preset
	if (_performance_mode && _current_preset >= 0)
	{
		ini_file preset(_preset_files[_current_preset]);

		for (auto &constant : module.spec_constants)
		{
			switch (constant.type.base)
			{
			case reshadefx::type::t_int:
				preset.get(path.filename().u8string(), constant.name, constant.initializer_value.as_int);
				break;
			case reshadefx::type::t_uint:
				preset.get(path.filename().u8string(), constant.name, constant.initializer_value.as_uint);
				break;
			case reshadefx::type::t_float:
				preset.get(path.filename().u8string(), constant.name, constant.initializer_value.as_float);
				break;
			}
		}
	}

	std::lock_guard<std::mutex> lock(_reload_mutex);

	const size_t storage_base_offset = _uniform_data_storage.size();

	for (const reshadefx::uniform_info &info : module.uniforms)
	{
		uniform &variable = _uniforms.emplace_back(info);
		variable.effect_filename = path.filename().u8string();
		variable.hidden = variable.annotations["hidden"].second.as_uint[0];

		variable.storage_offset = storage_base_offset + variable.offset;
		// Create space for the new variable in the storage area and fill it with the initializer value
		_uniform_data_storage.resize(variable.storage_offset + variable.size);

		reset_uniform_value(variable);

		if (const auto it = variable.annotations.find("source"); it != variable.annotations.end())
		{
			if (it->second.second.string_data == "frametime")
				variable.special = special_uniform::frame_time;
			else if (it->second.second.string_data == "framecount")
				variable.special = special_uniform::frame_count;
			else if (it->second.second.string_data == "random")
				variable.special = special_uniform::random;
			else if (it->second.second.string_data == "pingpong")
				variable.special = special_uniform::ping_pong;
			else if (it->second.second.string_data == "date")
				variable.special = special_uniform::date;
			else if (it->second.second.string_data == "timer")
				variable.special = special_uniform::timer;
			else if (it->second.second.string_data == "key")
				variable.special = special_uniform::key;
			else if (it->second.second.string_data == "mousepoint")
				variable.special = special_uniform::mouse_point;
			else if (it->second.second.string_data == "mousedelta")
				variable.special = special_uniform::mouse_delta;
			else if (it->second.second.string_data == "mousebutton")
				variable.special = special_uniform::mouse_button;
		}
	}

	for (const reshadefx::texture_info &info : module.textures)
	{
		if (const auto existing_texture = std::find_if(_textures.begin(), _textures.end(),
			[&texture_name = info.unique_name](const auto &item) { return item.unique_name == texture_name; });
			existing_texture != _textures.end())
		{
			if (info.semantic.empty() && (
				existing_texture->width != info.width ||
				existing_texture->height != info.height ||
				existing_texture->levels != info.levels ||
				existing_texture->format != info.format))
			{
				errors += "warning: " + existing_texture->effect_filename + " already created a texture with the same name but different dimensions; textures are shared across all effects, so either rename the variable or adjust the dimensions so they match\n";
			}

			continue;
		}

		if (!info.semantic.empty() && (info.semantic != "COLOR" && info.semantic != "DEPTH"))
			errors += "warning: " + info.unique_name + ": unknown semantic '" + info.semantic + "'\n";

		texture &texture = _textures.emplace_back(info);
		texture.effect_filename = path.filename().u8string();

		if (info.semantic == "COLOR")
			texture.impl_reference = texture_reference::back_buffer;
		else if (info.semantic == "DEPTH")
			texture.impl_reference = texture_reference::depth_buffer;
		else if (!info.semantic.empty())
			errors += "warning: " + info.unique_name + ": unknown semantic '" + info.semantic + "'\n";
	}

	for (const reshadefx::technique_info &info : module.techniques)
	{
		technique &technique = _techniques.emplace_back(info);
		technique.effect_index = _loaded_effects.size();
		technique.effect_filename = path.filename().u8string();
		technique.enabled = technique.annotations["enabled"].second.as_uint[0];
		technique.hidden = technique.annotations["hidden"].second.as_uint[0];
		technique.timeout = technique.annotations["timeout"].second.as_int[0];
		technique.timeleft = technique.timeout;
		technique.toggle_key_data[0] = technique.annotations["toggle"].second.as_uint[0];
		technique.toggle_key_data[1] = technique.annotations["togglectrl"].second.as_uint[0] ? 1 : 0;
		technique.toggle_key_data[2] = technique.annotations["toggleshift"].second.as_uint[0] ? 1 : 0;
		technique.toggle_key_data[3] = technique.annotations["togglealt"].second.as_uint[0] ? 1 : 0;
	}

	if (errors.empty())
		LOG(INFO) << "Successfully compiled " << path << ".";
	else
		LOG(WARNING) << "Successfully compiled " << path << " with warnings:\n" << errors;

	effect_data effect;
	effect.errors = std::move(errors);
	effect.module = std::move(module);
	effect.source_file = path;
	effect.storage_offset = storage_base_offset;

	const auto roundto16 = [](size_t size) { return (size + 15) & ~15; };
	effect.storage_size = roundto16(_uniform_data_storage.size() - storage_base_offset);
	_uniform_data_storage.resize(effect.storage_offset + effect.storage_size);

	_loaded_effects.push_back(std::move(effect));

	if (--_reload_remaining_effects == 0)
	{
		_last_reload_time = std::chrono::high_resolution_clock::now();
		_has_finished_reloading = true;
	}
}
void reshade::runtime::load_textures()
{
	LOG(INFO) << "Loading image files for textures ...";

	for (texture &texture : _textures)
	{
		if (texture.impl == nullptr || texture.impl_reference != texture_reference::none)
			continue; // Ignore textures that are not created yet and those that are handled in the runtime implementation

		const auto source = texture.annotations.find("source");
		if (source == texture.annotations.end())
			continue; // Ignore textures that have no image file attached to them (e.g. plain render targets)

		// Search for image file using the provided search paths
		std::error_code ec;
		std::filesystem::path path;
		for (const auto &search_path : _texture_search_paths)
			if (std::filesystem::exists(path = search_path / source->second.second.string_data, ec))
				break;

		if (!std::filesystem::exists(path, ec))
		{
			LOG(ERROR) << "> Source " << path << " for texture '" << texture.unique_name << "' could not be found.";
			continue;
		}

		unsigned char *filedata = nullptr;
		int width = 0, height = 0, channels = 0;

		if (FILE *file; _wfopen_s(&file, path.wstring().c_str(), L"rb") == 0)
		{
			if (stbi_dds_test_file(file))
				filedata = stbi_dds_load_from_file(file, &width, &height, &channels, STBI_rgb_alpha);
			else
				filedata = stbi_load_from_file(file, &width, &height, &channels, STBI_rgb_alpha);

			fclose(file);
		}

		if (filedata == nullptr)
		{
			LOG(ERROR) << "> Source " << path << " for texture '" << texture.unique_name << "' could not be loaded! Make sure it is of a compatible file format.";
			continue;
		}

		if (texture.width != uint32_t(width) ||
			texture.height != uint32_t(height))
		{
			LOG(INFO) << "> Resizing image data for texture '" << texture.unique_name << "' from " << width << "x" << height << " to " << texture.width << "x" << texture.height << " ...";

			std::vector<uint8_t> resized(texture.width * texture.height * 4);
			stbir_resize_uint8(filedata, width, height, 0, resized.data(), texture.width, texture.height, 0, 4);
			update_texture(texture, resized.data());
		}
		else
		{
			update_texture(texture, filedata);
		}

		stbi_image_free(filedata);
	}
}

void reshade::runtime::enable_technique(technique &technique)
{
	technique.enabled = true;
	technique.timeleft = technique.timeout;

	// Queue effect file for compilation if it was not fully loaded yet
	if (technique.impl == nullptr)
		_reload_queue.push_back(technique.effect_index);
}
void reshade::runtime::disable_technique(technique &technique)
{
	technique.enabled = false;
	technique.timeleft = 0;
	technique.average_cpu_duration.clear();
	technique.average_gpu_duration.clear();
}

void reshade::runtime::load_config()
{
	const ini_file config(_configuration_path);

	config.get("INPUT", "KeyScreenshot", _screenshot_key_data);
	config.get("INPUT", "KeyReload", _reload_key_data);
	config.get("INPUT", "KeyEffects", _effects_key_data);

	config.get("GENERAL", "PerformanceMode", _performance_mode);
	config.get("GENERAL", "EffectSearchPaths", _effect_search_paths);
	config.get("GENERAL", "TextureSearchPaths", _texture_search_paths);
	config.get("GENERAL", "PreprocessorDefinitions", _preprocessor_definitions);
	config.get("GENERAL", "PresetFiles", _preset_files);
	config.get("GENERAL", "CurrentPreset", _current_preset);
	config.get("GENERAL", "ScreenshotPath", _screenshot_path);
	config.get("GENERAL", "ScreenshotFormat", _screenshot_format);
	config.get("GENERAL", "ScreenshotIncludePreset", _screenshot_include_preset);
	config.get("GENERAL", "ScreenshotIncludeConfiguration", _screenshot_include_configuration);
	config.get("GENERAL", "NoReloadOnInit", _no_reload_on_init);

	// Make sure the preset index is in-bounds
	if (_current_preset >= static_cast<ptrdiff_t>(_preset_files.size()))
		_current_preset = -1;

	// Look for new preset files in the main directory
	const std::filesystem::path parent_path = g_reshade_dll_path.parent_path();
	std::error_code ec;
	for (const auto &entry : std::filesystem::directory_iterator(parent_path, ec))
	{
		const std::filesystem::path preset_file = entry.path();
		if (preset_file.extension() != ".ini" && preset_file.extension() != ".txt")
			continue; // Only look at INI and TXT files
		if (std::find_if(_preset_files.begin(), _preset_files.end(),
			[&preset_file, &parent_path](const auto &path) {
				return preset_file.filename() == path.filename() && (path.parent_path() == parent_path || !path.is_absolute());
			}) != _preset_files.end())
			continue; // Preset file is already in the preset list

		// Check if the INI file contains a list of techniques (it is not a valid preset file if it does not)
		const ini_file preset(preset_file);

		std::vector<std::string> techniques;
		preset.get("", "Techniques", techniques);

		if (!techniques.empty())
			_preset_files.push_back(preset_file);
	}

	for (const auto &callback : _load_config_callables)
		callback(config);
}
void reshade::runtime::save_config() const
{
	save_config(_configuration_path);
}
void reshade::runtime::save_config(const std::filesystem::path &save_path) const
{
	ini_file config(_configuration_path, save_path);

	config.set("INPUT", "KeyScreenshot", _screenshot_key_data);
	config.set("INPUT", "KeyReload", _reload_key_data);
	config.set("INPUT", "KeyEffects", _effects_key_data);

	config.set("GENERAL", "PerformanceMode", _performance_mode);
	config.set("GENERAL", "EffectSearchPaths", _effect_search_paths);
	config.set("GENERAL", "TextureSearchPaths", _texture_search_paths);
	config.set("GENERAL", "PreprocessorDefinitions", _preprocessor_definitions);
	config.set("GENERAL", "PresetFiles", _preset_files);
	config.set("GENERAL", "CurrentPreset", _current_preset);
	config.set("GENERAL", "ScreenshotPath", _screenshot_path);
	config.set("GENERAL", "ScreenshotFormat", _screenshot_format);
	config.set("GENERAL", "ScreenshotIncludePreset", _screenshot_include_preset);
	config.set("GENERAL", "ScreenshotIncludeConfiguration", _screenshot_include_configuration);
	config.set("GENERAL", "NoReloadOnInit", _no_reload_on_init);

	for (const auto &callback : _save_config_callables)
		callback(config);
}

void reshade::runtime::load_preset(const std::filesystem::path &path)
{
	ini_file preset(path);

	// Reorder techniques
	std::vector<std::string> technique_list;
	preset.get("", "Techniques", technique_list);
	std::vector<std::string> technique_sorting_list;
	preset.get("", "TechniqueSorting", technique_sorting_list);

	if (technique_sorting_list.empty())
		technique_sorting_list = technique_list;

	std::sort(_techniques.begin(), _techniques.end(),
		[&technique_sorting_list](const auto &lhs, const auto &rhs) {
			return (std::find(technique_sorting_list.begin(), technique_sorting_list.end(), lhs.name) - technique_sorting_list.begin()) <
			       (std::find(technique_sorting_list.begin(), technique_sorting_list.end(), rhs.name) - technique_sorting_list.begin());
		});

	for (uniform &variable : _uniforms)
	{
		reshadefx::constant values;

		switch (variable.type.base)
		{
		case reshadefx::type::t_int:
			get_uniform_value(variable, values.as_int, 16);
			preset.get(variable.effect_filename, variable.name, values.as_int);
			set_uniform_value(variable, values.as_int, 16);
			break;
		case reshadefx::type::t_bool:
		case reshadefx::type::t_uint:
			get_uniform_value(variable, values.as_uint, 16);
			preset.get(variable.effect_filename, variable.name, values.as_uint);
			set_uniform_value(variable, values.as_uint, 16);
			break;
		case reshadefx::type::t_float:
			get_uniform_value(variable, values.as_float, 16);
			preset.get(variable.effect_filename, variable.name, values.as_float);
			set_uniform_value(variable, values.as_float, 16);
			break;
		}
	}

	for (technique &technique : _techniques)
	{
		// Ignore preset if "enabled" annotation is set
		if (technique.annotations["enabled"].second.as_uint[0]
			|| std::find(technique_list.begin(), technique_list.end(), technique.name) != technique_list.end())
			enable_technique(technique);
		else
			disable_technique(technique);

		preset.get("", "Key" + technique.name, technique.toggle_key_data);
	}
}
void reshade::runtime::load_current_preset()
{
	if (_current_preset >= 0)
		load_preset(_preset_files[_current_preset]);
}
void reshade::runtime::save_preset(const std::filesystem::path &path) const
{
	save_preset(path, path);
}
void reshade::runtime::save_preset(const std::filesystem::path &path, const std::filesystem::path &save_path) const
{
	ini_file preset(path, save_path);

	std::vector<std::string> technique_list, technique_sorting_list;
	std::unordered_set<std::string> active_effect_filenames;

	for (const technique &technique : _techniques)
	{
		if (technique.enabled)
		{
			technique_list.push_back(technique.name);
			active_effect_filenames.insert(technique.effect_filename);
		}

		technique_sorting_list.push_back(technique.name);

		if (technique.toggle_key_data[0] != 0)
			preset.set("", "Key" + technique.name, technique.toggle_key_data);
		else if (int value = 0; preset.get("", "Key" + technique.name, value), value != 0)
			preset.set("", "Key" + technique.name, 0); // Clear toggle key data
	}

	preset.set("", "Techniques", std::move(technique_list));
	preset.set("", "TechniqueSorting", std::move(technique_sorting_list));

	for (const uniform &variable : _uniforms)
	{
		if (variable.special != special_uniform::none || !active_effect_filenames.count(variable.effect_filename))
			continue;

		assert(variable.type.components() < 16);

		reshadefx::constant values;

		switch (variable.type.base)
		{
		case reshadefx::type::t_int:
			get_uniform_value(variable, values.as_int, 16);
			preset.set(variable.effect_filename, variable.name, variant(values.as_int, variable.type.components()));
			break;
		case reshadefx::type::t_bool:
		case reshadefx::type::t_uint:
			get_uniform_value(variable, values.as_uint, 16);
			preset.set(variable.effect_filename, variable.name, variant(values.as_uint, variable.type.components()));
			break;
		case reshadefx::type::t_float:
			get_uniform_value(variable, values.as_float, 16);
			preset.set(variable.effect_filename, variable.name, variant(values.as_float, variable.type.components()));
			break;
		}
	}
}
void reshade::runtime::save_current_preset() const
{
	if (_current_preset >= 0)
		save_preset(_preset_files[_current_preset]);
}

void reshade::runtime::save_screenshot() const
{
	std::vector<uint8_t> data(_width * _height * 4);
	capture_frame(data.data());

	const int hour = _date[3] / 3600;
	const int minute = (_date[3] - hour * 3600) / 60;
	const int second = _date[3] - hour * 3600 - minute * 60;

	char filename[21];
	sprintf_s(filename, " %.4d-%.2d-%.2d %.2d-%.2d-%.2d", _date[0], _date[1], _date[2], hour, minute, second);
	const std::wstring least = _screenshot_path / g_target_executable_path.stem().concat(filename);
	const std::wstring screenshot_path = least + (_screenshot_format == 0 ? L".bmp" : L".png");

	LOG(INFO) << "Saving screenshot to " << screenshot_path << " ...";

	bool success = false;

	if (FILE *file; _wfopen_s(&file, screenshot_path.c_str(), L"wb") == 0)
	{
		const auto write_callback = [](void *context, void *data, int size) {
			fwrite(data, 1, size, static_cast<FILE *>(context));
		};

		switch (_screenshot_format)
		{
		case 0:
			success = stbi_write_bmp_to_func(write_callback, file, _width, _height, 4, data.data()) != 0;
			break;
		case 1:
			success = stbi_write_png_to_func(write_callback, file, _width, _height, 4, data.data(), 0) != 0;
			break;
		}

		fclose(file);
	}

	if (!success)
	{
		LOG(ERROR) << "Failed to write screenshot to " << screenshot_path << '!';
		return;
	}

	if (_screenshot_include_preset && _current_preset >= 0)
	{
		const std::filesystem::path preset_file = _preset_files[_current_preset];
		save_preset(preset_file, least + L' ' + preset_file.stem().wstring() + L".ini");
	}
	if (_screenshot_include_configuration)
	{
		save_config(least + L".ini");
	}
}

void reshade::runtime::get_uniform_value(const uniform &variable, uint8_t *data, size_t size) const
{
	assert(data != nullptr);

	size = std::min(size, size_t(variable.size));

	assert(variable.storage_offset + size <= _uniform_data_storage.size());

	std::memcpy(data, &_uniform_data_storage[variable.storage_offset], size);
}
void reshade::runtime::get_uniform_value(const uniform &variable, bool *values, size_t count) const
{
	count = std::min(count, size_t(variable.size / 4));

	assert(values != nullptr);

	const auto data = static_cast<uint8_t *>(alloca(variable.size));
	get_uniform_value(variable, data, variable.size);

	for (size_t i = 0; i < count; i++)
		values[i] = reinterpret_cast<const uint32_t *>(data)[i] != 0;
}
void reshade::runtime::get_uniform_value(const uniform &variable, int32_t *values, size_t count) const
{
	if (!variable.type.is_floating_point())
	{
		get_uniform_value(variable, reinterpret_cast<uint8_t *>(values), count * sizeof(int32_t));
		return;
	}

	count = std::min(count, variable.size / sizeof(float));

	assert(values != nullptr);

	const auto data = static_cast<uint8_t *>(alloca(variable.size));
	get_uniform_value(variable, data, variable.size);

	for (size_t i = 0; i < count; i++)
		values[i] = static_cast<int32_t>(reinterpret_cast<const float *>(data)[i]);
}
void reshade::runtime::get_uniform_value(const uniform &variable, uint32_t *values, size_t count) const
{
	get_uniform_value(variable, reinterpret_cast<int32_t *>(values), count);
}
void reshade::runtime::get_uniform_value(const uniform &variable, float *values, size_t count) const
{
	if (variable.type.is_floating_point())
	{
		get_uniform_value(variable, reinterpret_cast<uint8_t *>(values), count * sizeof(float));
		return;
	}

	count = std::min(count, variable.size / sizeof(int32_t));

	assert(values != nullptr);

	const auto data = static_cast<uint8_t *>(alloca(variable.size));
	get_uniform_value(variable, data, variable.size);

	for (size_t i = 0; i < count; ++i)
		if (variable.type.is_signed())
			values[i] = static_cast<float>(reinterpret_cast<const int32_t *>(data)[i]);
		else
			values[i] = static_cast<float>(reinterpret_cast<const uint32_t *>(data)[i]);
}
void reshade::runtime::set_uniform_value(uniform &variable, const uint8_t *data, size_t size)
{
	assert(data != nullptr);

	size = std::min(size, size_t(variable.size));

	assert(variable.storage_offset + size <= _uniform_data_storage.size());

	std::memcpy(&_uniform_data_storage[variable.storage_offset], data, size);
}
void reshade::runtime::set_uniform_value(uniform &variable, const bool *values, size_t count)
{
	const auto data = static_cast<uint8_t *>(alloca(count * 4));
	switch (variable.type.base)
	{
	case reshadefx::type::t_bool:
		for (size_t i = 0; i < count; ++i)
			reinterpret_cast<int32_t *>(data)[i] = values[i] ? -1 : 0;
		break;
	case reshadefx::type::t_int:
	case reshadefx::type::t_uint:
		for (size_t i = 0; i < count; ++i)
			reinterpret_cast<int32_t *>(data)[i] = values[i] ? 1 : 0;
		break;
	case reshadefx::type::t_float:
		for (size_t i = 0; i < count; ++i)
			reinterpret_cast<float *>(data)[i] = values[i] ? 1.0f : 0.0f;
		break;
	}

	set_uniform_value(variable, data, count * 4);
}
void reshade::runtime::set_uniform_value(uniform &variable, const int32_t *values, size_t count)
{
	if (!variable.type.is_floating_point())
	{
		set_uniform_value(variable, reinterpret_cast<const uint8_t *>(values), count * sizeof(int));
		return;
	}

	const auto data = static_cast<float *>(alloca(count * sizeof(float)));
	for (size_t i = 0; i < count; ++i)
		data[i] = static_cast<float>(values[i]);

	set_uniform_value(variable, reinterpret_cast<const uint8_t *>(data), count * sizeof(float));
}
void reshade::runtime::set_uniform_value(uniform &variable, const uint32_t *values, size_t count)
{
	if (!variable.type.is_floating_point())
	{
		set_uniform_value(variable, reinterpret_cast<const uint8_t *>(values), count * sizeof(int));
		return;
	}

	const auto data = static_cast<float *>(alloca(count * sizeof(float)));
	for (size_t i = 0; i < count; ++i)
		data[i] = static_cast<float>(values[i]);

	set_uniform_value(variable, reinterpret_cast<const uint8_t *>(data), count * sizeof(float));
}
void reshade::runtime::set_uniform_value(uniform &variable, const float *values, size_t count)
{
	if (variable.type.is_floating_point())
	{
		set_uniform_value(variable, reinterpret_cast<const uint8_t *>(values), count * sizeof(float));
		return;
	}

	const auto data = static_cast<int32_t *>(alloca(count * sizeof(int32_t)));
	for (size_t i = 0; i < count; ++i)
		data[i] = static_cast<int32_t>(values[i]);

	set_uniform_value(variable, reinterpret_cast<const uint8_t *>(data), count * sizeof(int32_t));
}

void reshade::runtime::reset_uniform_value(uniform &variable)
{
	if (variable.has_initializer_value)
		memcpy(_uniform_data_storage.data() + variable.storage_offset, variable.initializer_value.as_uint, variable.size);
	else
		memset(_uniform_data_storage.data() + variable.storage_offset, 0, variable.size);
}
