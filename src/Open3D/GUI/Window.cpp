// ----------------------------------------------------------------------------
// -                        Open3D: www.open3d.org                            -
// ----------------------------------------------------------------------------
// The MIT License (MIT)
//
// Copyright (c) 2018 www.open3d.org
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

#include "Open3D/GUI/Window.h"

#include <GLFW/glfw3.h>
#include <filament/Engine.h>
#include <imgui.h>
#include <imgui_internal.h>  // so we can examine the current context
#include <cmath>
#include <queue>
#include <unordered_map>
#include <vector>

#include "Open3D/GUI/Application.h"
#include "Open3D/GUI/Button.h"
#include "Open3D/GUI/Dialog.h"
#include "Open3D/GUI/ImguiFilamentBridge.h"
#include "Open3D/GUI/Label.h"
#include "Open3D/GUI/Layout.h"
#include "Open3D/GUI/Menu.h"
#include "Open3D/GUI/Native.h"
#include "Open3D/GUI/SceneWidget.h"
#include "Open3D/GUI/Theme.h"
#include "Open3D/GUI/Util.h"
#include "Open3D/GUI/Widget.h"
#include "Open3D/Utility/Console.h"
#include "Open3D/Visualization/Rendering/Filament/FilamentEngine.h"
#include "Open3D/Visualization/Rendering/Filament/FilamentRenderer.h"

using namespace open3d::gui::util;

// ----------------------------------------------------------------------------
namespace open3d {
namespace gui {

namespace {

static constexpr int CENTERED_X = -10000;
static constexpr int CENTERED_Y = -10000;
static constexpr int AUTOSIZE_WIDTH = 0;
static constexpr int AUTOSIZE_HEIGHT = 0;

// Assumes the correct ImGuiContext is current
void UpdateImGuiForScaling(float new_scaling) {
    ImGuiStyle& style = ImGui::GetStyle();
    // FrameBorderSize is not adjusted (we want minimal borders)
    style.FrameRounding *= new_scaling;
}

int MouseButtonFromGLFW(int button) {
    switch (button) {
        case GLFW_MOUSE_BUTTON_LEFT:
            return int(MouseButton::LEFT);
        case GLFW_MOUSE_BUTTON_RIGHT:
            return int(MouseButton::RIGHT);
        case GLFW_MOUSE_BUTTON_MIDDLE:
            return int(MouseButton::MIDDLE);
        case GLFW_MOUSE_BUTTON_4:
            return int(MouseButton::BUTTON4);
        case GLFW_MOUSE_BUTTON_5:
            return int(MouseButton::BUTTON5);
        default:
            return int(MouseButton::NONE);
    }
}

int KeymodsFromGLFW(int glfw_mods) {
    int keymods = 0;
    if (glfw_mods & GLFW_MOD_SHIFT) {
        keymods |= int(KeyModifier::SHIFT);
    }
    if (glfw_mods & GLFW_MOD_CONTROL) {
#if __APPLE__
        keymods |= int(KeyModifier::ALT);
#else
        keymods |= int(KeyModifier::CTRL);
#endif  // __APPLE__
    }
    if (glfw_mods & GLFW_MOD_ALT) {
#if __APPLE__
        keymods |= int(KeyModifier::META);
#else
        keymods |= int(KeyModifier::ALT);
#endif  // __APPLE__
    }
    if (glfw_mods & GLFW_MOD_SUPER) {
#if __APPLE__
        keymods |= int(KeyModifier::CTRL);
#else
        keymods |= int(KeyModifier::META);
#endif  // __APPLE__
    }
    return keymods;
}

void ChangeAllRenderQuality(
        SceneWidget::Quality quality,
        const std::vector<std::shared_ptr<Widget>>& children) {
    for (auto child : children) {
        auto sw = std::dynamic_pointer_cast<SceneWidget>(child);
        if (sw) {
            sw->SetRenderQuality(quality);
        } else {
            if (child->GetChildren().size() > 0) {
                ChangeAllRenderQuality(quality, child->GetChildren());
            }
        }
    }
}

}  // namespace

const int Window::FLAG_TOPMOST = (1 << 0);

struct Window::Impl {
    GLFWwindow* window_ = nullptr;
    std::string title_;  // there is no glfwGetWindowTitle()...
    // We need these for mouse moves and wheel events.
    // The only source of ground truth is button events, so the rest of
    // the time we monitor key up/down events.
    int mouse_mods_ = 0;  // ORed KeyModifiers
    double last_render_time_ = 0.0;

    Theme theme_;  // so that the font size can be different based on scaling
    std::unique_ptr<visualization::FilamentRenderer> renderer_;
    struct {
        std::unique_ptr<ImguiFilamentBridge> imgui_bridge;
        ImGuiContext* context = nullptr;
        ImFont* system_font = nullptr;  // reference; owned by imguiContext
        float scaling = 1.0;
    } imgui_;
    std::vector<std::shared_ptr<Widget>> children_;

    // Active dialog is owned here. It is not put in the children because
    // we are going to add it and take it out during draw (since that's
    // how an immediate mode GUI works) and that involves changing the
    // children while iterating over it. Also, conceptually it is not a
    // child, it is a child window, and needs to be on top, which we cannot
    // guarantee if it is a child widget.
    std::shared_ptr<Dialog> active_dialog_;

    std::queue<std::function<void()>> deferred_until_before_draw_;
    std::queue<std::function<void()>> deferred_until_draw_;
    Widget* mouse_grabber_widget_ = nullptr;  // only if not ImGUI widget
    Widget* focus_widget_ =
            nullptr;  // only used if ImGUI isn't taking keystrokes
    bool wants_auto_size_and_center_ = false;
    bool needs_layout_ = true;
    bool is_resizing_ = false;
};

Window::Window(const std::string& title, int flags /*= 0*/)
    : Window(title, CENTERED_X, CENTERED_Y, AUTOSIZE_WIDTH, AUTOSIZE_HEIGHT) {}

Window::Window(const std::string& title,
               int width,
               int height,
               int flags /*= 0*/)
    : Window(title, CENTERED_X, CENTERED_Y, width, height) {}

Window::Window(const std::string& title,
               int x,
               int y,
               int width,
               int height,
               int flags /*= 0*/)
    : impl_(new Window::Impl()) {
    if (x == CENTERED_X || y == CENTERED_Y || width == AUTOSIZE_WIDTH ||
        height == AUTOSIZE_HEIGHT) {
        impl_->wants_auto_size_and_center_ = true;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    // NOTE: Setting alpha and stencil bits to match GLX standard default
    // values. GLFW sets these internally to 8 and 8 respectively if not
    // specified which causes problems with Filament on Linux with Nvidia binary
    // driver
    glfwWindowHint(GLFW_ALPHA_BITS, 0);
    glfwWindowHint(GLFW_STENCIL_BITS, 0);

#if __APPLE__
    glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
#endif
    glfwWindowHint(GLFW_VISIBLE,
                   impl_->wants_auto_size_and_center_ ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_FLOATING,
                   ((flags & FLAG_TOPMOST) != 0 ? GLFW_TRUE : GLFW_FALSE));

    impl_->window_ = glfwCreateWindow(std::max(10, width), std::max(10, height),
                                      title.c_str(), NULL, NULL);
    impl_->title_ = title;

    if (x != CENTERED_X || y != CENTERED_Y) {
        glfwSetWindowPos(impl_->window_, x, y);
    }

    glfwSetWindowUserPointer(impl_->window_, this);
    glfwSetWindowSizeCallback(impl_->window_, ResizeCallback);
    glfwSetWindowRefreshCallback(impl_->window_, DrawCallback);
    glfwSetCursorPosCallback(impl_->window_, MouseMoveCallback);
    glfwSetMouseButtonCallback(impl_->window_, MouseButtonCallback);
    glfwSetScrollCallback(impl_->window_, MouseScrollCallback);
    glfwSetKeyCallback(impl_->window_, KeyCallback);
    glfwSetCharCallback(impl_->window_, CharCallback);
    glfwSetDropCallback(impl_->window_, DragDropCallback);
    glfwSetWindowCloseCallback(impl_->window_, CloseCallback);

    // On single-threaded platforms, Filament's OpenGL context must be current,
    // not GLFW's context, so create the renderer after the window.

    // ImGUI creates a bitmap atlas from a font, so we need to have the correct
    // size when we create it, because we can't change the bitmap without
    // reloading the whole thing (expensive).
    float scaling = GetScaling();
    impl_->theme_ = Application::GetInstance().GetTheme();
    impl_->theme_.font_size *= scaling;
    impl_->theme_.default_margin *= scaling;
    impl_->theme_.default_layout_spacing *= scaling;

    auto& engine = visualization::EngineInstance::GetInstance();
    auto& resource_manager =
            visualization::EngineInstance::GetResourceManager();

    impl_->renderer_ = std::make_unique<visualization::FilamentRenderer>(
            engine, GetNativeDrawable(), resource_manager);

    auto& theme = impl_->theme_;  // shorter alias
    impl_->imgui_.context = ImGui::CreateContext();
    auto oldContext = MakeDrawContextCurrent();

    impl_->imgui_.imgui_bridge = std::make_unique<ImguiFilamentBridge>(
            impl_->renderer_.get(), GetSize());

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(0, 0);
    style.WindowRounding = 0;
    style.WindowBorderSize = 0;
    style.FrameBorderSize = theme.border_width;
    style.FrameRounding = theme.border_radius;
    style.Colors[ImGuiCol_WindowBg] = colorToImgui(theme.background_color);
    style.Colors[ImGuiCol_Text] = colorToImgui(theme.text_color);
    style.Colors[ImGuiCol_Border] = colorToImgui(theme.border_color);
    style.Colors[ImGuiCol_Button] = colorToImgui(theme.button_color);
    style.Colors[ImGuiCol_ButtonHovered] =
            colorToImgui(theme.button_hover_color);
    style.Colors[ImGuiCol_ButtonActive] =
            colorToImgui(theme.button_active_color);
    style.Colors[ImGuiCol_CheckMark] = colorToImgui(theme.checkbox_check_color);
    style.Colors[ImGuiCol_FrameBg] =
            colorToImgui(theme.combobox_background_color);
    style.Colors[ImGuiCol_FrameBgHovered] =
            colorToImgui(theme.combobox_hover_color);
    style.Colors[ImGuiCol_FrameBgActive] =
            style.Colors[ImGuiCol_FrameBgHovered];
    style.Colors[ImGuiCol_SliderGrab] = colorToImgui(theme.slider_grab_color);
    style.Colors[ImGuiCol_SliderGrabActive] =
            colorToImgui(theme.slider_grab_color);
    style.Colors[ImGuiCol_Tab] = colorToImgui(theme.tab_inactive_color);
    style.Colors[ImGuiCol_TabHovered] = colorToImgui(theme.tab_hover_color);
    style.Colors[ImGuiCol_TabActive] = colorToImgui(theme.tab_active_color);

    // If the given font path is invalid, ImGui will silently fall back to
    // proggy, which is a tiny "pixel art" texture that is compiled into the
    // library.
    if (!theme.font_path.empty()) {
        ImGuiIO& io = ImGui::GetIO();
        impl_->imgui_.system_font = io.Fonts->AddFontFromFileTTF(
                theme.font_path.c_str(), theme.font_size);
        /*static*/ unsigned char* pixels;
        int textureW, textureH, bytesPerPx;
        io.Fonts->GetTexDataAsAlpha8(&pixels, &textureW, &textureH,
                                     &bytesPerPx);
        impl_->imgui_.imgui_bridge->CreateAtlasTextureAlpha8(
                pixels, textureW, textureH, bytesPerPx);
        ImGui::SetCurrentFont(impl_->imgui_.system_font);
    }

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
#ifdef WIN32
    io.ImeWindowHandle = GetNativeDrawable();
#endif
    // ImGUI's io.KeysDown is indexed by our scan codes, and we fill out
    // io.KeyMap to map from our code to ImGui's code.
    io.KeyMap[ImGuiKey_Tab] = KEY_TAB;
    io.KeyMap[ImGuiKey_LeftArrow] = KEY_LEFT;
    io.KeyMap[ImGuiKey_RightArrow] = KEY_RIGHT;
    io.KeyMap[ImGuiKey_UpArrow] = KEY_UP;
    io.KeyMap[ImGuiKey_DownArrow] = KEY_DOWN;
    io.KeyMap[ImGuiKey_PageUp] = KEY_PAGEUP;
    io.KeyMap[ImGuiKey_PageDown] = KEY_PAGEDOWN;
    io.KeyMap[ImGuiKey_Home] = KEY_HOME;
    io.KeyMap[ImGuiKey_End] = KEY_END;
    io.KeyMap[ImGuiKey_Insert] = KEY_INSERT;
    io.KeyMap[ImGuiKey_Delete] = KEY_DELETE;
    io.KeyMap[ImGuiKey_Backspace] = KEY_BACKSPACE;
    io.KeyMap[ImGuiKey_Space] = ' ';
    io.KeyMap[ImGuiKey_Enter] = KEY_ENTER;
    io.KeyMap[ImGuiKey_Escape] = KEY_ESCAPE;
    io.KeyMap[ImGuiKey_A] = 'a';
    io.KeyMap[ImGuiKey_C] = 'c';
    io.KeyMap[ImGuiKey_V] = 'v';
    io.KeyMap[ImGuiKey_X] = 'x';
    io.KeyMap[ImGuiKey_Y] = 'y';
    io.KeyMap[ImGuiKey_Z] = 'z';
    /*    io.SetClipboardTextFn = [this](void*, const char* text) {
            glfwSetClipboardString(this->impl_->window, text);
        };
        io.GetClipboardTextFn = [this](void*) -> const char* {
            return glfwGetClipboardString(this->impl_->window);
        }; */
    io.ClipboardUserData = nullptr;

    // Restore the context, in case we are creating a window during a draw.
    // (This is quite likely, since ImGUI only handles things like button
    // presses during draw. A file open dialog is likely to create a window
    // after pressing "Open".)
    RestoreDrawContext(oldContext);
}

Window::~Window() {
    impl_->children_.clear();  // needs to happen before deleting renderer
    ImGui::SetCurrentContext(impl_->imgui_.context);
    ImGui::DestroyContext();
    impl_->renderer_.reset();
    glfwDestroyWindow(impl_->window_);
}

void* Window::MakeDrawContextCurrent() const {
    auto old_context = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(impl_->imgui_.context);
    return old_context;
}

void Window::RestoreDrawContext(void* oldContext) const {
    ImGui::SetCurrentContext((ImGuiContext*)oldContext);
}

void* Window::GetNativeDrawable() const {
    return open3d::gui::GetNativeDrawable(impl_->window_);
}

const Theme& Window::GetTheme() const { return impl_->theme_; }

visualization::Renderer& Window::GetRenderer() const {
    return *impl_->renderer_;
}

Rect Window::GetOSFrame() const {
    int x, y, w, h;
    glfwGetWindowPos(impl_->window_, &x, &y);
    glfwGetWindowSize(impl_->window_, &w, &h);
    return Rect(x, y, w, h);
}

void Window::SetOSFrame(const Rect& r) {
    glfwSetWindowPos(impl_->window_, r.x, r.y);
    glfwSetWindowSize(impl_->window_, r.width, r.height);
}

const char* Window::GetTitle() const { return impl_->title_.c_str(); }

void Window::SetTitle(const char* title) {
    impl_->title_ = title;
    glfwSetWindowTitle(impl_->window_, title);
}

// Note: can only be called if the ImGUI context is current (that is,
//       after MakeDrawContextCurrent() has been called), otherwise
//       ImGUI won't be able to access the font.
Size Window::CalcPreferredSize() {
    Rect bbox(0, 0, 0, 0);
    for (auto& child : impl_->children_) {
        auto pref = child->CalcPreferredSize(GetTheme());
        Rect r(child->GetFrame().x, child->GetFrame().y, pref.width,
               pref.height);
        bbox = bbox.UnionedWith(r);
    }

    // Note: we are doing (bbox.GetRight() - 0) NOT (bbox.GetRight() - bbox.x)
    //       (and likewise for height) because the origin of the window is
    //       (0, 0) and anything up/left is clipped.
    return Size(bbox.GetRight(), bbox.GetBottom());
}

void Window::SizeToFit() {
    // CalcPreferredSize() can only be called while the ImGUI context
    // is current, but we are probably calling this while setting up the
    // window.
    auto auto_size = [this]() { SetSize(CalcPreferredSize()); };
    impl_->deferred_until_draw_.push(auto_size);
}

void Window::SetSize(const Size& size) {
    // Make sure we do the resize outside of a draw, to avoid unsightly
    // errors if we happen to do this in the middle of a draw.
    auto resize = [this, size /*copy*/]() {
        glfwSetWindowSize(this->impl_->window_,
                          size.width / this->impl_->imgui_.scaling,
                          size.height / this->impl_->imgui_.scaling);
        // SDL_SetWindowSize() doesn't generate an event, so we need to update
        // the size ourselves
        this->OnResize();
    };
    impl_->deferred_until_before_draw_.push(resize);
}

Size Window::GetSize() const {
    uint32_t w, h;
    glfwGetFramebufferSize(impl_->window_, (int*)&w, (int*)&h);
    return Size(w, h);
}

Rect Window::GetContentRect() const {
    auto size = GetSize();
    int menu_height = 0;
#if !(GUI_USE_NATIVE_MENUS && defined(__APPLE__))
    MakeDrawContextCurrent();
    auto menubar = Application::GetInstance().GetMenubar();
    if (menubar) {
        menu_height = menubar->CalcHeight(GetTheme());
    }
#endif

    return Rect(0, menu_height, size.width, size.height - menu_height);
}

float Window::GetScaling() const {
#if GLFW_VERSION_MAJOR > 3 || \
        (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 3)
    float xscale, yscale;
    glfwGetWindowContentScale(impl_->window_, &xscale, &yscale);
    return xscale;
#else
    return 1.0f;
#endif  // GLFW version >= 3.3
}

Point Window::GlobalToWindowCoord(int global_x, int global_y) {
    int wx, wy;
    glfwGetWindowPos(impl_->window_, &wx, &wy);
    return Point(global_y - wx, global_y - wy);
}

bool Window::IsVisible() const {
    return glfwGetWindowAttrib(impl_->window_, GLFW_VISIBLE);
}

void Window::Show(bool vis /*= true*/) {
    if (vis) {
        glfwShowWindow(impl_->window_);
    } else {
        glfwHideWindow(impl_->window_);
    }
}

void Window::Close() { Application::GetInstance().RemoveWindow(this); }

void Window::SetNeedsLayout() { impl_->needs_layout_ = true; }

void Window::PostRedraw() { PostNativeExposeEvent(impl_->window_); }

void Window::RaiseToTop() const { glfwFocusWindow(impl_->window_); }

bool Window::IsActiveWindow() const {
    return glfwGetWindowAttrib(impl_->window_, GLFW_FOCUSED);
}

void Window::SetFocusWidget(Widget* w) { impl_->focus_widget_ = w; }

void Window::AddChild(std::shared_ptr<Widget> w) {
    impl_->children_.push_back(w);
    impl_->needs_layout_ = true;
}

void Window::ShowDialog(std::shared_ptr<Dialog> dlg) {
    if (impl_->active_dialog_) {
        CloseDialog();
    }
    impl_->active_dialog_ = dlg;
    dlg->OnWillShow();

    auto win_size = GetSize();
    auto pref = dlg->CalcPreferredSize(GetTheme());
    int w = dlg->GetFrame().width;
    int h = dlg->GetFrame().height;
    if (w == 0) {
        w = pref.width;
    }
    if (h == 0) {
        h = pref.height;
    }
    w = std::min(w, int(std::round(0.8 * win_size.width)));
    h = std::min(h, int(std::round(0.8 * win_size.height)));
    dlg->SetFrame(gui::Rect((win_size.width - w) / 2, (win_size.height - h) / 2,
                            w, h));
    dlg->Layout(GetTheme());
}

void Window::CloseDialog() {
    if (impl_->focus_widget_ == impl_->active_dialog_.get()) {
        SetFocusWidget(nullptr);
    }
    impl_->active_dialog_.reset();
}

void Window::ShowMessageBox(const char* title, const char* message) {
    auto em = GetTheme().font_size;
    auto margins = Margins(GetTheme().default_margin);
    auto dlg = std::make_shared<Dialog>(title);
    auto layout = std::make_shared<Vert>(em, margins);
    layout->AddChild(std::make_shared<Label>(message));
    auto ok = std::make_shared<Button>("Ok");
    ok->SetOnClicked([this]() { this->CloseDialog(); });
    layout->AddChild(Horiz::MakeCentered(ok));
    dlg->AddChild(layout);
    ShowDialog(dlg);
}

void Window::Layout(const Theme& theme) {
    if (impl_->children_.size() == 1) {
        auto r = GetContentRect();
        impl_->children_[0]->SetFrame(r);
        impl_->children_[0]->Layout(theme);
    } else {
        for (auto& child : impl_->children_) {
            child->Layout(theme);
        }
    }
}

void Window::OnMenuItemSelected(Menu::ItemId item_id) {}

namespace {
enum Mode { NORMAL, DIALOG, NO_INPUT };

Widget::DrawResult DrawChild(DrawContext& dc,
                             const char* name,
                             std::shared_ptr<Widget> child,
                             Mode mode) {
    // Note: ImGUI's concept of a "window" is really a moveable child of the
    //       OS window. We want a child to act like a child of the OS window,
    //       like native UI toolkits, Qt, etc. So the top-level widgets of
    //       a window are drawn using ImGui windows whose frame is specified
    //       and which have no title bar, resizability, etc.

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoCollapse;
    // Q: When we want no input, why not use ImGui::BeginPopupModal(),
    //    which takes care of blocking input for us, since a modal popup
    //    is the most likely use case for wanting no input?
    // A: It animates an overlay, which would require us to constantly
    //    redraw, otherwise it only animates when the mouse moves. But
    //    we don't need constant animation for anything else, so that would
    //    be a waste of CPU and battery (and really annoys people like me).
    if (mode == NO_INPUT) {
        flags |= ImGuiWindowFlags_NoInputs;
    }
    auto frame = child->GetFrame();
    bool bg_color_not_default = !child->IsDefaultBackgroundColor();
    auto is_container = !child->GetChildren().empty();
    if (is_container) {
        dc.uiOffsetX = frame.x;
        dc.uiOffsetY = frame.y;
        ImGui::SetNextWindowPos(ImVec2(frame.x, frame.y));
        ImGui::SetNextWindowSize(ImVec2(frame.width, frame.height));
        if (bg_color_not_default) {
            auto& bgColor = child->GetBackgroundColor();
            ImGui::PushStyleColor(ImGuiCol_WindowBg,
                                  util::colorToImgui(bgColor));
        }
        ImGui::Begin(name, nullptr, flags);
    } else {
        dc.uiOffsetX = 0;
        dc.uiOffsetY = 0;
    }

    Widget::DrawResult result;
    result = child->Draw(dc);

    if (is_container) {
        ImGui::End();
        if (bg_color_not_default) {
            ImGui::PopStyleColor();
        }
    }

    return result;
}
}  // namespace

Widget::DrawResult Window::DrawOnce(bool is_layout_pass) {
    // These are here to provide fast unique window names. (Hence using
    // char* instead of a std::string, just in case c_str() recreates
    // the buffer on some platform and unwittingly makes
    // ImGui::DrawChild(dc, name.c_str(), ...) slow.
    // If you find yourself needing more than a handful of top-level
    // children, you should probably be using a layout of some sort
    // (gui::Vert, gui::Horiz, gui::VGrid, etc. See Layout.h).
    static const std::vector<const char*> win_names = {
            "win1",  "win2",  "win3",  "win4",  "win5",  "win6",  "win7",
            "win8",  "win9",  "win10", "win11", "win12", "win13", "win14",
            "win15", "win16", "win17", "win18", "win19", "win20"};

    bool needs_layout = false;
    bool needs_redraw = false;

    // ImGUI uses the dt parameter to calculate double-clicks, so it
    // needs to be reasonably accurate.
    double now = Application::GetInstance().Now();
    float dt_sec = now - impl_->last_render_time_;
    impl_->last_render_time_ = now;

    // Run the deferred callbacks that need to happen outside a draw
    while (!impl_->deferred_until_before_draw_.empty()) {
        impl_->deferred_until_before_draw_.front()();
        impl_->deferred_until_before_draw_.pop();
    }

    // Set current context
    MakeDrawContextCurrent();  // make sure our ImGUI context is active
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = dt_sec;

    // Set mouse information
    io.MousePos = ImVec2(-FLT_MAX, -FLT_MAX);
    if (IsActiveWindow()) {
        double mx, my;
        glfwGetCursorPos(impl_->window_, &mx, &my);
        auto scaling = GetScaling();
        io.MousePos = ImVec2(mx * scaling, my * scaling);
    }
    io.MouseDown[0] =
            (glfwGetMouseButton(impl_->window_, GLFW_MOUSE_BUTTON_LEFT) ==
             GLFW_PRESS);
    io.MouseDown[1] =
            (glfwGetMouseButton(impl_->window_, GLFW_MOUSE_BUTTON_RIGHT) ==
             GLFW_PRESS);
    io.MouseDown[2] =
            (glfwGetMouseButton(impl_->window_, GLFW_MOUSE_BUTTON_MIDDLE) ==
             GLFW_PRESS);

    // Set key information
    io.KeyShift = (impl_->mouse_mods_ & int(KeyModifier::SHIFT));
    io.KeyAlt = (impl_->mouse_mods_ & int(KeyModifier::ALT));
    io.KeyCtrl = (impl_->mouse_mods_ & int(KeyModifier::CTRL));
    io.KeySuper = (impl_->mouse_mods_ & int(KeyModifier::META));

    // Begin an ImGUI frame. We should NOT begin a filament frame here:
    // a) ImGUI always needs to "draw", because event processing happens
    //    during draw for immediate mode GUIs, but if this is a layout
    //    pass (as ImGUI can take up two draws to layout widgets and text)
    //    we aren't actually going to render it.
    // b) Filament pumps events during a beginFrame(), which can cause
    //    a key up event to process and erase the key down state from
    //    the ImGuiIO structure before we get a chance to draw/process it.
    ImGui::NewFrame();
    ImGui::PushFont(impl_->imgui_.system_font);

    // Run the deferred callbacks that need to happen inside a draw
    // In particular, text sizing with ImGUI seems to require being
    // in a frame, otherwise there isn't an GL texture info and we crash.
    while (!impl_->deferred_until_draw_.empty()) {
        impl_->deferred_until_draw_.front()();
        impl_->deferred_until_draw_.pop();
    }

    // Layout if necessary.  This must happen within ImGui setup so that widgets
    // can query font information.
    auto& theme = impl_->theme_;
    if (impl_->needs_layout_) {
        Layout(theme);
        impl_->needs_layout_ = false;
    }

    auto size = GetSize();
    int em = theme.font_size;  // em = font size in digital type (see Wikipedia)
    DrawContext dc{theme,      *impl_->renderer_, 0,  0,
                   size.width, size.height,       em, dt_sec};

    // Draw all the widgets. These will get recorded by ImGui.
    size_t win_idx = 0;
    Mode draw_mode = (impl_->active_dialog_ ? NO_INPUT : NORMAL);
    for (auto& child : this->impl_->children_) {
        if (!child->IsVisible()) {
            continue;
        }
        if (win_idx >= win_names.size()) {
            win_idx = win_names.size() - 1;
            utility::LogWarning(
                    "Using too many top-level child widgets; use a layout "
                    "instead.");
        }
        auto result = DrawChild(dc, win_names[win_idx++], child, draw_mode);
        if (result != Widget::DrawResult::NONE) {
            needs_redraw = true;
        }
        if (result == Widget::DrawResult::RELAYOUT) {
            needs_layout = true;
        }
    }

    // Draw menubar after the children so it is always on top (although it
    // shouldn't matter, as there shouldn't be anything under it)
    auto menubar = Application::GetInstance().GetMenubar();
    if (menubar) {
        auto id = menubar->DrawMenuBar(dc, !impl_->active_dialog_);
        if (id != Menu::NO_ITEM) {
            OnMenuItemSelected(id);
            needs_redraw = true;
        }
    }

    // Draw any active dialog
    if (impl_->active_dialog_) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,
                            theme.dialog_border_width);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,
                            theme.dialog_border_radius);
        if (DrawChild(dc, "dialog", impl_->active_dialog_, DIALOG) !=
            Widget::DrawResult::NONE) {
            needs_redraw = true;
        }
        ImGui::PopStyleVar(2);
    }

    // Finish frame and generate the commands
    ImGui::PopFont();
    ImGui::EndFrame();
    ImGui::Render();  // creates the draw data (i.e. Render()s to data)

    // Draw the ImGui commands
    impl_->imgui_.imgui_bridge->Update(ImGui::GetDrawData());

    // Draw. Since ImGUI is an immediate mode gui, it does layout during
    // draw, and if we are drawing for layout purposes, don't actually
    // draw, because we are just going to draw again after this returns.
    if (!is_layout_pass) {
        impl_->renderer_->BeginFrame();
        impl_->renderer_->Draw();
        impl_->renderer_->EndFrame();
    }

    if (needs_layout) {
        return Widget::DrawResult::RELAYOUT;
    } else if (needs_redraw) {
        return Widget::DrawResult::REDRAW;
    } else {
        return Widget::DrawResult::NONE;
    }
}

Window::DrawResult Window::OnDraw() {
    bool needed_layout = impl_->needs_layout_;

    auto result = DrawOnce(needed_layout);
    if (result == Widget::DrawResult::RELAYOUT) {
        impl_->needs_layout_ = true;
    }

    // ImGUI can take two frames to do its layout, so if we did a layout
    // redraw a second time. This helps prevent a brief red flash when the
    // window first appears, as well as corrupted images if the
    // window initially appears underneath the mouse.
    if (needed_layout || impl_->needs_layout_) {
        DrawOnce(false);
    }

    return (result == Widget::DrawResult::NONE ? NONE : REDRAW);
}

void Window::OnResize() {
    impl_->needs_layout_ = true;

#if __APPLE__
    // We need to recreate the swap chain after resizing a window on macOS
    // otherwise things look very wrong.
    impl_->renderer_->UpdateSwapChain();
#endif  // __APPLE__

    impl_->imgui_.imgui_bridge->OnWindowResized(*this);

    auto size = GetSize();
    auto scaling = GetScaling();

    auto old_context = MakeDrawContextCurrent();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(size.width, size.height);
    if (impl_->imgui_.scaling != scaling) {
        UpdateImGuiForScaling(1.0 / impl_->imgui_.scaling);  // undo previous
        UpdateImGuiForScaling(scaling);
        impl_->imgui_.scaling = scaling;
    }
    io.DisplayFramebufferScale.x = 1.0f;
    io.DisplayFramebufferScale.y = 1.0f;

    if (impl_->wants_auto_size_and_center_) {
        impl_->wants_auto_size_and_center_ = false;
        int screen_width = 1024;  // defaults in case monitor == nullptr
        int screen_height = 768;
        auto* monitor = glfwGetWindowMonitor(impl_->window_);
        if (monitor) {
            const GLFWvidmode* mode = glfwGetVideoMode(monitor);
            if (mode) {
                screen_width = mode->width;
                screen_height = mode->height;
            }
        }
        ImGui::NewFrame();
        ImGui::PushFont(impl_->imgui_.system_font);
        auto pref = CalcPreferredSize();
        Size size(pref.width / this->impl_->imgui_.scaling,
                  pref.height / this->impl_->imgui_.scaling);
        glfwSetWindowSize(impl_->window_, size.width, size.height);
        glfwSetWindowPos(impl_->window_, (screen_width - size.width) / 2,
                         (screen_height - size.height) / 2);
        ImGui::PopFont();
        ImGui::EndFrame();
        OnResize();
    }

    // Resizing looks bad if drawing takes a long time, so turn off MSAA
    // while we resize. On macOS this is critical, because the GL driver does
    // not release the memory for all the buffers of the new sizes right away
    // so it eats up GBs of memory rapidly and then resizing looks awful and
    // eventually stops working correctly. Unfortunately, there isn't a good
    // way to tell when we've stopped resizing, so we use the mouse movement.
    // (We get no mouse events while resizing, so any mouse even must mean we
    // are no longer resizing.)
    if (!impl_->is_resizing_) {
        impl_->is_resizing_ = true;
        ChangeAllRenderQuality(SceneWidget::Quality::FAST, impl_->children_);
    }

    RestoreDrawContext(old_context);
}

void Window::OnMouseEvent(const MouseEvent& e) {
    MakeDrawContextCurrent();

    // We don't have a good way of determining when resizing ends; the most
    // likely action after resizing a window is to move the mouse.
    if (impl_->is_resizing_) {
        impl_->is_resizing_ = false;
        ChangeAllRenderQuality(SceneWidget::Quality::BEST, impl_->children_);
    }

    impl_->mouse_mods_ = e.modifiers;

    switch (e.type) {
        case MouseEvent::MOVE:
        case MouseEvent::BUTTON_DOWN:
        case MouseEvent::DRAG:
        case MouseEvent::BUTTON_UP:
            break;
        case MouseEvent::WHEEL: {
            ImGuiIO& io = ImGui::GetIO();
            float dx = 0.0, dy = 0.0;
            if (e.wheel.dx != 0) {
                dx = e.wheel.dx / std::abs(e.wheel.dx);  // get sign
            }
            if (e.wheel.dy != 0) {
                dy = e.wheel.dy / std::abs(e.wheel.dy);  // get sign
            }
            // Note: ImGUI's documentation says that 1 unit of wheel movement
            //       is about 5 lines of text scrolling.
            if (e.wheel.isTrackpad) {
                io.MouseWheelH += dx * 0.25;
                io.MouseWheel += dy * 0.25;
            } else {
                io.MouseWheelH += dx;
                io.MouseWheel += dy;
            }
            break;
        }
    }

    if (impl_->mouse_grabber_widget_) {
        impl_->mouse_grabber_widget_->Mouse(e);
        if (e.type == MouseEvent::BUTTON_UP) {
            impl_->mouse_grabber_widget_ = nullptr;
        }
        return;
    }

    // Some ImGUI widgets have popup windows, in particular, the color
    // picker, which creates a popup window when you click on the color
    // patch. Since these aren't gui::Widgets, we don't know about them,
    // and will deliver mouse events to something below them. So find any
    // that would use the mouse, and if it isn't a toplevel child, then
    // eat the event for it.
    if (e.type == MouseEvent::BUTTON_DOWN || e.type == MouseEvent::BUTTON_UP) {
        ImGuiContext* context = ImGui::GetCurrentContext();
        for (auto* w : context->Windows) {
            if (!w->Hidden && w->Flags & ImGuiWindowFlags_Popup) {
                Rect r(w->Pos.x, w->Pos.y, w->Size.x, w->Size.y);
                if (r.Contains(e.x, e.y)) {
                    bool weKnowThis = false;
                    for (auto child : impl_->children_) {
                        if (child->GetFrame() == r) {
                            weKnowThis = true;
                            break;
                        }
                    }
                    if (!weKnowThis) {
                        // This is not a rect that is one of our children,
                        // must be an ImGUI internal popup. Eat event.
                        return;
                    }
                }
            }
        }
    }

    // Iterate backwards so that we send mouse events from the top down.
    auto HandleMouseForChild = [this](const MouseEvent& e,
                                      std::shared_ptr<Widget> child) -> bool {
        if (child->GetFrame().Contains(e.x, e.y) && child->IsVisible()) {
            if (e.type == MouseEvent::BUTTON_DOWN) {
                SetFocusWidget(child.get());
            }
            auto result = child->Mouse(e);
            if (e.type == MouseEvent::BUTTON_DOWN) {
                if (result == Widget::EventResult::CONSUMED) {
                    impl_->mouse_grabber_widget_ = child.get();
                }
            } else if (e.type == MouseEvent::BUTTON_UP) {
                impl_->mouse_grabber_widget_ = nullptr;
            }
            return true;
        }
        return false;
    };
    if (impl_->active_dialog_) {
        HandleMouseForChild(e, impl_->active_dialog_);
    } else {
        // Mouse move and wheel always get delivered.
        // Button up and down get delivered if they weren't in an ImGUI popup.
        // Drag should only be delivered if the grabber widget exists;
        // if it is null, then the mouse is being dragged over an ImGUI popup.
        if (e.type != MouseEvent::DRAG || impl_->mouse_grabber_widget_) {
            std::vector<std::shared_ptr<Widget>>& children = impl_->children_;
            for (auto it = children.rbegin(); it != children.rend(); ++it) {
                if (HandleMouseForChild(e, *it)) {
                    break;
                }
            }
        }
    }
}

void Window::OnKeyEvent(const KeyEvent& e) {
    auto this_mod = 0;
    if (e.key == KEY_LSHIFT || e.key == KEY_RSHIFT) {
        this_mod = int(KeyModifier::SHIFT);
    } else if (e.key == KEY_LCTRL || e.key == KEY_RCTRL) {
        this_mod = int(KeyModifier::CTRL);
    } else if (e.key == KEY_ALT) {
        this_mod = int(KeyModifier::ALT);
    } else if (e.key == KEY_META) {
        this_mod = int(KeyModifier::META);
    }

    if (e.type == KeyEvent::UP) {
        impl_->mouse_mods_ &= ~this_mod;
    } else {
        impl_->mouse_mods_ |= this_mod;
    }

    auto old_context = MakeDrawContextCurrent();
    ImGuiIO& io = ImGui::GetIO();
    if (e.key < IM_ARRAYSIZE(io.KeysDown)) {
        io.KeysDown[e.key] = (e.type == KeyEvent::DOWN);
    }

    // If an ImGUI widget is not getting keystrokes, we can send them to
    // non-ImGUI widgets
    if (ImGui::GetCurrentContext()->ActiveId == 0 && impl_->focus_widget_) {
        impl_->focus_widget_->Key(e);
    }

    RestoreDrawContext(old_context);
}

void Window::OnTextInput(const TextInputEvent& e) {
    auto old_context = MakeDrawContextCurrent();
    ImGuiIO& io = ImGui::GetIO();
    io.AddInputCharactersUTF8(e.utf8);
    RestoreDrawContext(old_context);
}

bool Window::OnTickEvent(const TickEvent& e) {
    auto old_context = MakeDrawContextCurrent();
    bool redraw = false;
    for (auto child : impl_->children_) {
        if (child->Tick(e) == Widget::DrawResult::REDRAW) {
            redraw = true;
        }
    }
    RestoreDrawContext(old_context);
    return redraw;
}

void Window::OnDragDropped(const char* path) {}

// ----------------------------------------------------------------------------
void Window::DrawCallback(GLFWwindow* window) {
    Window* w = static_cast<Window*>(glfwGetWindowUserPointer(window));
    if (w->OnDraw() == Window::REDRAW) {
        // Can't just draw here, because Filament sometimes fences within
        // a draw, and then you can get two draws happening at the same
        // time, which ends up with a crash.
        PostNativeExposeEvent(w->impl_->window_);
    }
}

void Window::ResizeCallback(GLFWwindow* window, int os_width, int os_height) {
    Window* w = static_cast<Window*>(glfwGetWindowUserPointer(window));
    w->OnResize();
    UpdateAfterEvent(w);
}

void Window::RescaleCallback(GLFWwindow* window, float xscale, float yscale) {
    Window* w = static_cast<Window*>(glfwGetWindowUserPointer(window));
    w->OnResize();
    UpdateAfterEvent(w);
}

void Window::MouseMoveCallback(GLFWwindow* window, double x, double y) {
    Window* w = static_cast<Window*>(glfwGetWindowUserPointer(window));
    int buttons = 0;
    for (int b = GLFW_MOUSE_BUTTON_1; b < GLFW_MOUSE_BUTTON_5; ++b) {
        if (glfwGetMouseButton(window, b) == GLFW_PRESS) {
            buttons |= MouseButtonFromGLFW(b);
        }
    }
    float scaling = w->GetScaling();
    int ix = int(std::ceil(x * scaling));
    int iy = int(std::ceil(y * scaling));

    auto type = (buttons == 0 ? MouseEvent::MOVE : MouseEvent::DRAG);
    MouseEvent me = {type, ix, iy, w->impl_->mouse_mods_};
    me.button.button = MouseButton(buttons);

    w->OnMouseEvent(me);
    UpdateAfterEvent(w);
}

void Window::MouseButtonCallback(GLFWwindow* window,
                                 int button,
                                 int action,
                                 int mods) {
    Window* w = static_cast<Window*>(glfwGetWindowUserPointer(window));

    auto type = (action == GLFW_PRESS ? MouseEvent::BUTTON_DOWN
                                      : MouseEvent::BUTTON_UP);
    double mx, my;
    glfwGetCursorPos(window, &mx, &my);
    float scaling = w->GetScaling();
    int ix = int(std::ceil(mx * scaling));
    int iy = int(std::ceil(my * scaling));

    MouseEvent me = {type, ix, iy, KeymodsFromGLFW(mods)};
    me.button.button = MouseButton(MouseButtonFromGLFW(button));

    w->OnMouseEvent(me);
    UpdateAfterEvent(w);
}

void Window::MouseScrollCallback(GLFWwindow* window, double dx, double dy) {
    Window* w = static_cast<Window*>(glfwGetWindowUserPointer(window));

    double mx, my;
    glfwGetCursorPos(window, &mx, &my);
    float scaling = w->GetScaling();
    int ix = int(std::ceil(mx * scaling));
    int iy = int(std::ceil(my * scaling));

    MouseEvent me = {MouseEvent::WHEEL, ix, iy, w->impl_->mouse_mods_};
    me.wheel.dx = dx;
    me.wheel.dy = dy;

    // GLFW doesn't give us any information about whether this scroll event
    // came from a mousewheel or a trackpad two-finger scroll.
#if __APPLE__
    me.wheel.isTrackpad = true;
#else
    me.wheel.isTrackpad = false;
#endif  // __APPLE__

    w->OnMouseEvent(me);
    UpdateAfterEvent(w);
}

void Window::KeyCallback(
        GLFWwindow* window, int key, int scancode, int action, int mods) {
    static std::unordered_map<int, uint32_t> g_GLFW2Key = {
            {GLFW_KEY_BACKSPACE, KEY_BACKSPACE},
            {GLFW_KEY_TAB, KEY_TAB},
            {GLFW_KEY_ENTER, KEY_ENTER},
            {GLFW_KEY_ESCAPE, KEY_ESCAPE},
            {GLFW_KEY_DELETE, KEY_DELETE},
            {GLFW_KEY_LEFT_SHIFT, KEY_LSHIFT},
            {GLFW_KEY_RIGHT_SHIFT, KEY_RSHIFT},
            {GLFW_KEY_LEFT_CONTROL, KEY_LCTRL},
            {GLFW_KEY_RIGHT_CONTROL, KEY_RCTRL},
            {GLFW_KEY_LEFT_ALT, KEY_ALT},
            {GLFW_KEY_RIGHT_ALT, KEY_ALT},
            {GLFW_KEY_LEFT_SUPER, KEY_META},
            {GLFW_KEY_RIGHT_SUPER, KEY_META},
            {GLFW_KEY_CAPS_LOCK, KEY_CAPSLOCK},
            {GLFW_KEY_LEFT, KEY_LEFT},
            {GLFW_KEY_RIGHT, KEY_RIGHT},
            {GLFW_KEY_UP, KEY_UP},
            {GLFW_KEY_DOWN, KEY_DOWN},
            {GLFW_KEY_INSERT, KEY_INSERT},
            {GLFW_KEY_HOME, KEY_HOME},
            {GLFW_KEY_END, KEY_END},
            {GLFW_KEY_PAGE_UP, KEY_PAGEUP},
            {GLFW_KEY_PAGE_DOWN, KEY_PAGEDOWN},
    };
    Window* w = static_cast<Window*>(glfwGetWindowUserPointer(window));

    auto type = (action == GLFW_RELEASE ? KeyEvent::Type::UP
                                        : KeyEvent::Type::DOWN);

    uint32_t k = key;
    if (key >= 'A' && key <= 'Z') {
        k += 32;  // GLFW gives uppercase for letters, convert to lowercase
    } else {
        auto it = g_GLFW2Key.find(key);
        if (it != g_GLFW2Key.end()) {
            k = it->second;
        }
    }
    KeyEvent e = {type, k, (action == GLFW_REPEAT)};

    w->OnKeyEvent(e);
    UpdateAfterEvent(w);
}

void Window::CharCallback(GLFWwindow* window, unsigned int utf32char) {
    // Convert utf-32 to utf8
    // From https://stackoverflow.com/a/42013433/218226
    // Note: This code handles all characters, but non-European characters
    //       won't draw unless we will include them in the ImGUI font (which
    //       is prohibitively large for hanzi/kanji)
    char utf8[5];
    if (utf32char <= 0x7f) {
        utf8[0] = utf32char;
        utf8[1] = '\0';
    } else if (utf32char <= 0x7ff) {
        utf8[0] = 0xc0 | (utf32char >> 6);
        utf8[1] = 0x80 | (utf32char & 0x3f);
        utf8[2] = '\0';
    } else if (utf32char <= 0xffff) {
        utf8[0] = 0xe0 | (utf32char >> 12);
        utf8[1] = 0x80 | ((utf32char >> 6) & 0x3f);
        utf8[2] = 0x80 | (utf32char & 0x3f);
        utf8[3] = '\0';
    } else if (utf32char <= 0x10ffff) {
        utf8[0] = 0xf0 | (utf32char >> 18);
        utf8[1] = 0x80 | ((utf32char >> 12) & 0x3f);
        utf8[2] = 0x80 | ((utf32char >> 6) & 0x3f);
        utf8[3] = 0x80 | (utf32char & 0x3f);
        utf8[4] = '\0';
    } else {
        // These characters are supposed to be forbidden, but just in case
        utf8[0] = '?';
        utf8[1] = '\0';
    }

    Window* w = static_cast<Window*>(glfwGetWindowUserPointer(window));
    w->OnTextInput(TextInputEvent{utf8});
    UpdateAfterEvent(w);
}

void Window::DragDropCallback(GLFWwindow* window,
                              int count,
                              const char* paths[]) {
    Window* w = static_cast<Window*>(glfwGetWindowUserPointer(window));
    for (int i = 0; i < count; ++i) {
        w->OnDragDropped(paths[i]);
    }
    UpdateAfterEvent(w);
}

void Window::CloseCallback(GLFWwindow* window) {
    Window* w = static_cast<Window*>(glfwGetWindowUserPointer(window));
    Application::GetInstance().RemoveWindow(w);
}

void Window::UpdateAfterEvent(Window* w) {
    PostNativeExposeEvent(w->impl_->window_);
}

}  // namespace gui
}  // namespace open3d
