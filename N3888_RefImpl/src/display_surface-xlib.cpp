#include "io2d.h"
#include "xio2dhelpers.h"
#include "xcairoenumhelpers.h"
#include <cairo-xlib.h>
#include <sstream>
#include <string>
#include <iostream>

// Only uncomment this if you need to do debugging. This will cause XSynchronize to be called for the _Display connection.
//#define USE_X_SYNC_FOR_DEBUGGING

using namespace std;
using namespace std::experimental::io2d;

Bool display_surface::_X11_if_event_pred(::Display* display, ::XEvent* event, XPointer arg) {
	assert(display != nullptr && event != nullptr && arg != nullptr);
	auto sfc = reinterpret_cast<display_surface*>(arg);
	// If the display_surface window is invalid, we will never get a match so return False.
	if (sfc->_Wndw == None) {
		return False;
	}
	// Need to check for ExposureMask events, StructureNotifyMask events, and unmaskable events.
	switch(event->type) {
	// ExposureMask evemts:
	case Expose:
	{
		if (event->xexpose.window == sfc->_Wndw) {
			return True;
		}
	} break;
	// StructureNotifyMask events:
	case CirculateNotify:
	{
		if (event->xcirculate.window == sfc->_Wndw) {
			return True;
		}
	} break;
	case ConfigureNotify:
	{
		if (event->xconfigure.window == sfc->_Wndw) {
			return True;
		}
	} break;
	case DestroyNotify:
	{
		if (event->xdestroywindow.window == sfc->_Wndw) {
			return True;
		}
	} break;
	case GravityNotify:
	{
		if (event->xgravity.window == sfc->_Wndw) {
			return True;
		}
	} break;
	case MapNotify:
	{
		if (event->xmap.window == sfc->_Wndw) {
			return True;
		}
	} break;
	case ReparentNotify:
	{
		if (event->xreparent.window == sfc->_Wndw) {
			return True;
		}
	} break;
	case UnmapNotify:
	{
		if (event->xunmap.window == sfc->_Wndw) {
			return True;
		}
	} break;
	// Might get them even though unrequested events (see http://www.x.org/releases/X11R7.7/doc/libX11/libX11/libX11.html#Event_Masks ):
	case GraphicsExpose:
	{
		if (event->xgraphicsexpose.drawable == static_cast<Drawable>(sfc->_Wndw)) {
			return True;
		}
	} break;
	case NoExpose:
	{
		if (event->xnoexpose.drawable == static_cast<Drawable>(sfc->_Wndw)) {
			return True;
		}
	} break;
	// Unmasked events
	case ClientMessage:
	{
		if (event->xclient.window == sfc->_Wndw) {
			return True;
		}
	} break;
	case MappingNotify:
	{
		if (event->xmapping.window == sfc->_Wndw) {
			return True;
		}
	} break;
	case SelectionClear:
	{
		if (event->xselectionclear.window == sfc->_Wndw) {
			return True;
		}
	} break;
	case SelectionNotify:
	{
		if (event->xselection.requestor == sfc->_Wndw) {
			return True;
		}
	} break;
	case SelectionRequest:
	{
		if (event->xselectionrequest.owner == sfc->_Wndw) {
			return True;
		}
	} break;
	default:
	{
		// Per the X protocol, types 64 through 127 are reserved for extensions.
		// We only care about non-extension events since we likely should be aware of those and should handle them.
		// So we only return True if it is not an extension event.
		if (event->type < 64 || event->type > 127) {
			// Return True so we can inspect it in the event loop for diagnostic purposes.
			return True;
		}
	}
	}
	return False;
}

display_surface::native_handle_type display_surface::native_handle() const {
	return{ { _Surface.get(), _Context.get() }, _Display.get(), _Wndw, _Display_mutex, _Display_ref_count };
}

display_surface::display_surface(display_surface&& other)
	: surface(move(other))
	, _Scaling(move(other._Scaling))
	, _Width(move(other._Width))
	, _Height(move(other._Height))
	, _Display_width(move(other._Display_width))
	, _Display_height(move(other._Display_height))
	, _Draw_fn(move(other._Draw_fn))
	, _Size_change_fn(move(other._Size_change_fn))
	, _Wndw(move(other._Wndw))
	, _Can_draw(move(other._Can_draw))
	, _Native_surface(move(other._Native_surface))
	, _Native_context(move(other._Native_context)) {
	other._Draw_fn = nullptr;
	other._Size_change_fn = nullptr;
	other._Wndw = None;
}

display_surface& display_surface::operator=(display_surface&& other) {
	if (this != &other) {
		surface::operator=(move(other));
		_Scaling = move(other._Scaling);
		_Width = move(other._Width);
		_Height = move(other._Height);
		_Display_width = move(other._Display_width);
		_Display_height = move(other._Display_height);
		_Draw_fn = move(other._Draw_fn);
		_Size_change_fn = move(other._Size_change_fn);
		_Wndw = move(other._Wndw);
		_Can_draw = move(other._Can_draw);
		_Native_surface = move(other._Native_surface);
		_Native_context = move(other._Native_context);

		other._Wndw = None;
		other._Draw_fn = nullptr;
		other._Size_change_fn = nullptr;
		other._Can_draw = false;
	}

	return *this;
}

mutex display_surface::_Display_mutex;
unique_ptr<Display, function<void(Display*)>> display_surface::_Display{ nullptr, [](Display*) { return; } };
int display_surface::_Display_ref_count = 0;

namespace {
	Atom _Wm_delete_window;
}

void display_surface::_Make_native_surface_and_context() {
	_Native_surface = unique_ptr<cairo_surface_t, function<void(cairo_surface_t*)>>(cairo_xlib_surface_create(_Display.get(), _Wndw, DefaultVisual(_Display.get(), DefaultScreen(_Display.get())), _Display_width, _Display_height), &cairo_surface_destroy);
	_Native_context = unique_ptr<cairo_t, decltype(&cairo_destroy)>(cairo_create(_Native_surface.get()), &cairo_destroy);
	_Throw_if_failed_cairo_status_t(cairo_surface_status(_Native_surface.get()));
	_Throw_if_failed_cairo_status_t(cairo_status(_Native_context.get()));
}

void display_surface::_Resize_window() {
	XWindowChanges xwc{ };
	xwc.width = _Display_width;
	xwc.height = _Display_height;
	XConfigureWindow(_Display.get(), _Wndw, CWWidth | CWHeight, &xwc);
}

display_surface::display_surface(int preferredWidth, int preferredHeight, experimental::io2d::format preferredFormat, scaling scl)
	: surface({ nullptr, nullptr }, preferredFormat, _Cairo_content_t_to_content(_Cairo_content_t_for_cairo_format_t(_Format_to_cairo_format_t(preferredFormat))))
	, _Scaling(scl)
	, _Width(preferredWidth)
	, _Height(preferredHeight)
	, _Display_width(preferredWidth)
	, _Display_height(preferredHeight)
	, _Draw_fn()
	, _Size_change_fn()
	, _Wndw(None)
	, _Can_draw(false)
	, _Native_surface(nullptr, &cairo_surface_destroy)
	, _Native_context(nullptr, &cairo_destroy) {
	Display* display = nullptr;
	// Lock to increment the ref count.
	{
		lock_guard<mutex> lg(_Display_mutex);
		_Display_ref_count++;
	}
	if (_Display == nullptr) {
		lock_guard<mutex> lg(_Display_mutex);
		if (_Display == nullptr) {
			display = XOpenDisplay(nullptr);
			if (display == nullptr) {
				_Display_ref_count--;
				throw system_error(make_error_code(errc::connection_refused));
			}
			_Display = unique_ptr<Display, decltype(&::XCloseDisplay)>(display, &XCloseDisplay);
			_Wm_delete_window = XInternAtom(display, "WM_DELETE_WINDOW", False);
			#ifdef USE_X_SYNC_FOR_DEBUGGING
			// DEBUGGING ONLY!!!!!!
			XSynchronize(display, True);
			#endif
		}
	}
	display = _Display.get();
	int screenNumber = DefaultScreen(display);
	int x = 0;
	int y = 0;
	unsigned int borderWidth = 4;
	_Wndw = XCreateSimpleWindow(display, RootWindow(display, screenNumber), x, y, static_cast<unsigned int>(preferredWidth), static_cast<unsigned int>(preferredHeight), borderWidth, WhitePixel(display, screenNumber), BlackPixel(display, screenNumber));
	XSelectInput(display, _Wndw, ExposureMask | StructureNotifyMask);
	XSetWMProtocols(display, _Wndw, &_Wm_delete_window, 1);
	XMapWindow(display, _Wndw);
	_Surface = unique_ptr<cairo_surface_t, function<void(cairo_surface_t*)>>(cairo_image_surface_create(_Format_to_cairo_format_t(_Format), _Width, _Height), &cairo_surface_destroy);
	_Context = unique_ptr<cairo_t, decltype(&cairo_destroy)>(cairo_create(_Surface.get()), &cairo_destroy);
}

display_surface::~display_surface() {
	_Native_context.reset();
	_Native_surface.reset();
	if (_Wndw != None) {
		XDestroyWindow(_Display.get(), _Wndw);
		_Wndw = None;
	}
	lock_guard<mutex> lg(_Display_mutex);
	_Display_ref_count--;
	assert(_Display_ref_count >= 0);
	if (_Display_ref_count <= 0) {
		_Display_ref_count = 0;
		_Display = nullptr;
	}
}

int display_surface::join() {
	bool exit = false;
	XEvent event;

	while (!exit) {
		while (XCheckIfEvent(_Display.get(), &event, &display_surface::_X11_if_event_pred, reinterpret_cast<XPointer>(this))) {
			switch(event.type) {
			// ExposureMask events:
			case Expose:
			{
				if (!_Can_draw && _Wndw != None) {
					_Make_native_surface_and_context();
				}
				assert(_Native_surface != nullptr && _Native_context != nullptr);
				_Can_draw = true;
				if (_Draw_fn != nullptr) {
					_Draw_fn(*this);
				}

				_Render_to_native_surface();
			} break;
			// StructureNotifyMask events:
			case CirculateNotify:
			{
			} break;
			case ConfigureNotify:
			{
				bool resized = false;
				if (event.xconfigure.width != _Display_width) {
					_Display_width = event.xconfigure.width;
					resized = true;
				}
				if (event.xconfigure.height != _Display_height) {
					_Display_height = event.xconfigure.height;
					resized = true;
				}
				if (resized) {
					cairo_xlib_surface_set_size(_Native_surface.get(), _Display_width, _Display_height);
				}
			} break;
			case DestroyNotify:
			{
				_Wndw = None;
				_Can_draw = false;
				_Native_context.reset();
				_Native_surface.reset();
				exit = true;
			} break;
			case GravityNotify:
			{
			} break;
			case MapNotify:
			{
			} break;
			case ReparentNotify:
			{
			} break;
			case UnmapNotify:
			{
				// The window still exists, it has just been unmapped.
				_Can_draw = false;
				_Native_context.reset();
				_Native_surface.reset();
			} break;
			// Might get them even though they are unrequested events (see http://www.x.org/releases/X11R7.7/doc/libX11/libX11/libX11.html#Event_Masks ):
			case GraphicsExpose:
			{
				if (_Can_draw) {
					if (_Draw_fn != nullptr) {
						_Draw_fn(*this);
					}

					_Render_to_native_surface();
				}
			} break;
			case NoExpose:
			{
			} break;
			// Unmasked events
			case ClientMessage:
			{
				if (event.xclient.format == 32 && static_cast<Atom>(event.xclient.data.l[0]) == _Wm_delete_window) {
					_Can_draw = false;
					_Native_context.reset();
					_Native_surface.reset();
					XDestroyWindow(_Display.get(), _Wndw);
					_Wndw = None;
					exit = true;
				}
				else {
					stringstream clientMsgStr;
					clientMsgStr << "ClientMessage event type '" << event.xclient.message_type << "' for unknown event type";
					auto atomName = XGetAtomName(_Display.get(), event.xclient.message_type);
					if (atomName != nullptr) {
						try {
							clientMsgStr << " (" << atomName << ")";
						}
						catch(...) {
							XFree(atomName);
						}
						XFree(atomName);
					}
					clientMsgStr << ". Format is '" << event.xclient.format << "' and first value is '";
					switch(event.xclient.format) {
					case 8:
					{
						clientMsgStr << to_string(static_cast<int>(event.xclient.data.b[0])).c_str();
					} break;
					case 16:
					{
						clientMsgStr << to_string(event.xclient.data.s[0]).c_str();
					} break;
					case 32:
					{
						clientMsgStr << to_string(event.xclient.data.l[0]).c_str();
					} break;
					default:
					{
						assert("Unexpected format." && false);
						clientMsgStr << "(unexpected format)";
					} break;
					}
					clientMsgStr << "'.";
					auto es = clientMsgStr.str().c_str();
					cerr << es << endl;
				}
			} break;
			case MappingNotify:
			{
			} break;
			case SelectionClear:
			{
			} break;
			case SelectionNotify:
			{
			} break;
			case SelectionRequest:
			{
			} break;
			default:
			{
				stringstream errorString;
				errorString << "Unexpected event.type. Value is '" << event.type << "'.";
				cerr << errorString.str().c_str();
				assert(event.type >= 64 && event.type <= 127);
			}
			}
		}
		if (_Can_draw) {
			// Run user draw function:
			if (_Draw_fn != nullptr) {
				_Draw_fn(*this);
			}

			_Render_to_native_surface();
		}
	}
	return 0;
}