// ----------------------------------------------------------------------------
// -                        Open3D: www.open3d.org                            -
// ----------------------------------------------------------------------------
// The MIT License (MIT)
//
// Copyright (c) 2015 Qianyi Zhou <Qianyi.Zhou@gmail.com>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
// ----------------------------------------------------------------------------

#include "DrawGeometry.h"

#include <Visualization/Visualizer/VisualizerWithCustomAnimation.h>
#include <Visualization/Visualizer/VisualizerWithKeyCallback.h>
#include <Visualization/Visualizer/ViewControlWithCustomAnimation.h>

namespace three{

bool DrawGeometry(
		std::shared_ptr<const Geometry> geometry_ptr,
		const std::string &window_name/* = "Open3D"*/, 
		const int width/* = 640*/, const int height/* = 480*/,
		const int left/* = 50*/, const int top/* = 50*/)
{
	std::vector<std::shared_ptr<const Geometry>> geometry_ptrs;
	geometry_ptrs.push_back(geometry_ptr);
	return DrawGeometries(geometry_ptrs, window_name, width, height, left, top);
}

bool DrawGeometryWithCustomAnimation(
		std::shared_ptr<const Geometry> geometry_ptr,
		const std::string &window_name/* = "Open3D"*/, 
		const int width/* = 640*/, const int height/* = 480*/,
		const int left/* = 50*/, const int top/* = 50*/,
		const std::string &json_filename/* = ""*/)
{
	std::vector<std::shared_ptr<const Geometry>> geometry_ptrs;
	geometry_ptrs.push_back(geometry_ptr);
	return DrawGeometriesWithCustomAnimation(geometry_ptrs, window_name, width, 
			height, left, top, json_filename);
}

bool DrawGeometryWithAnimationCallback(
		std::shared_ptr<const Geometry> geometry_ptr,
		std::function<bool(Visualizer &)> callback_func,
		const std::string &window_name/* = "Open3D"*/, 
		const int width/* = 640*/, const int height/* = 480*/,
		const int left/* = 50*/, const int top/* = 50*/)
{
	std::vector<std::shared_ptr<const Geometry>> geometry_ptrs;
	geometry_ptrs.push_back(geometry_ptr);
	return DrawGeometriesWithAnimationCallback(geometry_ptrs, callback_func,
			window_name, width, height, left, top);
}

bool DrawGeometryWithKeyCallback(
		std::shared_ptr<const Geometry> geometry_ptr,
		const int key, std::function<bool(Visualizer &)> callback_func,
		const std::string &window_name/* = "Open3D"*/, 
		const int width/* = 640*/, const int height/* = 480*/,
		const int left/* = 50*/, const int top/* = 50*/)
{
	std::vector<std::shared_ptr<const Geometry>> geometry_ptrs;
	geometry_ptrs.push_back(geometry_ptr);
	return DrawGeometriesWithKeyCallback(geometry_ptrs, key, callback_func,
			window_name, width, height, left, top);
}

bool DrawGeometries(
		const std::vector<std::shared_ptr<const Geometry>> &geometry_ptrs,
		const std::string &window_name/* = "Open3D"*/, 
		const int width/* = 640*/, const int height/* = 480*/,
		const int left/* = 50*/, const int top/* = 50*/)
{
	Visualizer visualizer;
	if (visualizer.CreateWindow(window_name, width, height, left, top) == 
			false) {
		PrintWarning("[DrawGeometry] Failed creating OpenGL window.\n");
		return false;
	}
	for (const auto &geometry_ptr : geometry_ptrs) {
		if (visualizer.AddGeometry(geometry_ptr) == false) {
			PrintWarning("[DrawGeometry] Failed adding geometry.\n");
			PrintWarning("[DrawGeometry] Possibly due to bad geometry or wrong geometry type.\n");
			return false;
		}
	}
	visualizer.Run();
	visualizer.DestroyWindow();
	return true;
}

bool DrawGeometriesWithCustomAnimation(
		const std::vector<std::shared_ptr<const Geometry>> &geometry_ptrs,
		const std::string &window_name/* = "Open3D"*/, 
		const int width/* = 640*/, const int height/* = 480*/,
		const int left/* = 50*/, const int top/* = 50*/,
		const std::string &json_filename/* = ""*/)
{
	VisualizerWithCustomAnimation visualizer;
	if (visualizer.CreateWindow(window_name, width, height, left, top) == 
			false) {
		PrintWarning("[DrawGeometry] Failed creating OpenGL window.\n");
		return false;
	}
	for (const auto &geometry_ptr : geometry_ptrs) {
		if (visualizer.AddGeometry(geometry_ptr) == false) {
			PrintWarning("[DrawGeometry] Failed adding geometry.\n");
			PrintWarning("[DrawGeometry] Possibly due to bad geometry or wrong geometry type.\n");
			return false;
		}
	}
	auto &view_control = 
			(ViewControlWithCustomAnimation &)visualizer.GetViewControl();
	if (json_filename.empty() == false) {
		if (view_control.LoadTrajectoryFromJsonFile(json_filename) == false) {
			PrintWarning("[DrawGeometry] Failed loading json file.\n");
			PrintWarning("[DrawGeometry] Possibly due to bad file or file does not contain trajectory.\n");
			return false;
		}
		visualizer.UpdateWindowTitle();
	}
	visualizer.Run();
	visualizer.DestroyWindow();
	return true;
}

bool DrawGeometriesWithAnimationCallback(
		const std::vector<std::shared_ptr<const Geometry>> &geometry_ptrs,
		std::function<bool(Visualizer &)> callback_func,
		const std::string &window_name/* = "Open3D"*/, 
		const int width/* = 640*/, const int height/* = 480*/,
		const int left/* = 50*/, const int top/* = 50*/)
{
	Visualizer visualizer;
	if (visualizer.CreateWindow(window_name, width, height, left, top) == 
			false) {
		PrintWarning("[DrawGeometry] Failed creating OpenGL window.\n");
		return false;
	}
	for (const auto &geometry_ptr : geometry_ptrs) {
		if (visualizer.AddGeometry(geometry_ptr) == false) {
			PrintWarning("[DrawGeometry] Failed adding geometry.\n");
			PrintWarning("[DrawGeometry] Possibly due to bad geometry or wrong geometry type.\n");
			return false;
		}
	}
	visualizer.RegisterAnimationCallback(callback_func);
	visualizer.Run();
	visualizer.DestroyWindow();
	return true;
}

bool DrawGeometriesWithKeyCallback(
		const std::vector<std::shared_ptr<const Geometry>> &geometry_ptrs,
		const int key, std::function<bool(Visualizer &)> callback_func,
		const std::string &window_name/* = "Open3D"*/, 
		const int width/* = 640*/, const int height/* = 480*/,
		const int left/* = 50*/, const int top/* = 50*/)
{
	VisualizerWithKeyCallback visualizer;
	if (visualizer.CreateWindow(window_name, width, height, left, top) == 
			false) {
		PrintWarning("[DrawGeometry] Failed creating OpenGL window.\n");
		return false;
	}
	for (const auto &geometry_ptr : geometry_ptrs) {
		if (visualizer.AddGeometry(geometry_ptr) == false) {
			PrintWarning("[DrawGeometry] Failed adding geometry.\n");
			PrintWarning("[DrawGeometry] Possibly due to bad geometry or wrong geometry type.\n");
			return false;
		}
	}
	visualizer.RegisterKeyCallback(key, callback_func);
	visualizer.Run();
	visualizer.DestroyWindow();
	return true;
}

}	// namespace three