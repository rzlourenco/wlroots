#define _POSIX_C_SOURCE 199309L
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <wlr/config.h>
#include <wlr/types/wlr_box.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_gamma_control.h>
#include <wlr/types/wlr_idle.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_wl_shell.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell_v6.h>
#include <wlr/util/log.h>
#include "rootston/seat.h"
#include "rootston/server.h"
#include "rootston/view.h"
#include "rootston/xcursor.h"

void view_get_box(const struct roots_view *view, struct wlr_box *box) {
	box->x = view->x;
	box->y = view->y;
	box->width = view->width;
	box->height = view->height;
}

void view_get_deco_box(const struct roots_view *view, struct wlr_box *box) {
	view_get_box(view, box);
	if (!view->decorated) {
		return;
	}

	box->x -= view->border_width;
	box->y -= (view->border_width + view->titlebar_height);
	box->width += view->border_width * 2;
	box->height += (view->border_width * 2 + view->titlebar_height);
}

enum roots_deco_part view_get_deco_part(struct roots_view *view, double sx, double sy) {
	if (!view->decorated) {
		return ROOTS_DECO_PART_NONE;
	}

	int sw = view->wlr_surface->current->width;
	int sh = view->wlr_surface->current->height;
	int bw = view->border_width;
	int titlebar_h = view->titlebar_height;

	if (sx > 0 && sx < sw && sy < 0 && sy > -view->titlebar_height) {
		return ROOTS_DECO_PART_TITLEBAR;
	}

	enum roots_deco_part parts = 0;
	if (sy >= -(titlebar_h + bw) &&
			sy <= sh + bw) {
		if (sx < 0 && sx > -bw) {
			parts |= ROOTS_DECO_PART_LEFT_BORDER;
		} else if (sx > sw && sx < sw + bw) {
			parts |= ROOTS_DECO_PART_RIGHT_BORDER;
		}
	}

	if (sx >= -bw && sx <= sw + bw) {
		if (sy > sh && sy <= sh + bw) {
			parts |= ROOTS_DECO_PART_BOTTOM_BORDER;
		} else if (sy >= -(titlebar_h + bw) && sy < 0) {
			parts |= ROOTS_DECO_PART_TOP_BORDER;
		}
	}

	// TODO corners

	return parts;
}

static void view_update_output(const struct roots_view *view,
		const struct wlr_box *before) {
	struct roots_desktop *desktop = view->desktop;
	struct roots_output *output;
	struct wlr_box box;
	view_get_box(view, &box);
	wl_list_for_each(output, &desktop->outputs, link) {
		bool intersected = before != NULL && wlr_output_layout_intersects(
			desktop->layout, output->wlr_output, before);
		bool intersects = wlr_output_layout_intersects(desktop->layout,
			output->wlr_output, &box);
		if (intersected && !intersects) {
			wlr_surface_send_leave(view->wlr_surface, output->wlr_output);
		}
		if (!intersected && intersects) {
			wlr_surface_send_enter(view->wlr_surface, output->wlr_output);
		}
	}
}

void view_move(struct roots_view *view, double x, double y) {
	if (view->x == x && view->y == y) {
		return;
	}

	struct wlr_box before;
	view_get_box(view, &before);
	if (view->move) {
		view->move(view, x, y);
	} else {
		view_update_position(view, x, y);
	}
	view_update_output(view, &before);
}

void view_activate(struct roots_view *view, bool activate) {
	if (view->activate) {
		view->activate(view, activate);
	}
}

void view_resize(struct roots_view *view, uint32_t width, uint32_t height) {
	struct wlr_box before;
	view_get_box(view, &before);
	if (view->resize) {
		view->resize(view, width, height);
	}
	view_update_output(view, &before);
}

void view_move_resize(struct roots_view *view, double x, double y,
		uint32_t width, uint32_t height) {
	bool update_x = x != view->x;
	bool update_y = y != view->y;
	if (!update_x && !update_y) {
		view_resize(view, width, height);
		return;
	}

	if (view->move_resize) {
		view->move_resize(view, x, y, width, height);
		return;
	}

	view->pending_move_resize.update_x = update_x;
	view->pending_move_resize.update_y = update_y;
	view->pending_move_resize.x = x;
	view->pending_move_resize.y = y;
	view->pending_move_resize.width = width;
	view->pending_move_resize.height = height;

	view_resize(view, width, height);
}

static struct wlr_output *view_get_output(struct roots_view *view) {
	struct wlr_box view_box;
	view_get_box(view, &view_box);

	double output_x, output_y;
	wlr_output_layout_closest_point(view->desktop->layout, NULL,
		view->x + (double)view_box.width/2,
		view->y + (double)view_box.height/2,
		&output_x, &output_y);
	return wlr_output_layout_output_at(view->desktop->layout, output_x,
		output_y);
}

void view_maximize(struct roots_view *view, bool maximized) {
	if (view->maximized == maximized) {
		return;
	}

	if (view->maximize) {
		view->maximize(view, maximized);
	}

	if (!view->maximized && maximized) {
		struct wlr_box view_box;
		view_get_box(view, &view_box);

		view->maximized = true;
		view->saved.x = view->x;
		view->saved.y = view->y;
		view->saved.rotation = view->rotation;
		view->saved.width = view_box.width;
		view->saved.height = view_box.height;

		struct wlr_output *output = view_get_output(view);
		struct wlr_box *output_box =
			wlr_output_layout_get_box(view->desktop->layout, output);

		view_move_resize(view, output_box->x, output_box->y, output_box->width,
			output_box->height);
		view_rotate(view, 0);
	}

	if (view->maximized && !maximized) {
		view->maximized = false;

		view_move_resize(view, view->saved.x, view->saved.y, view->saved.width,
			view->saved.height);
		view_rotate(view, view->saved.rotation);
	}
}

void view_set_fullscreen(struct roots_view *view, bool fullscreen,
		struct wlr_output *output) {
	bool was_fullscreen = view->fullscreen_output != NULL;
	if (was_fullscreen == fullscreen) {
		// TODO: support changing the output?
		return;
	}

	// TODO: check if client is focused?

	if (view->set_fullscreen) {
		view->set_fullscreen(view, fullscreen);
	}

	if (!was_fullscreen && fullscreen) {
		if (output == NULL) {
			output = view_get_output(view);
		}
		struct roots_output *roots_output =
			desktop_output_from_wlr_output(view->desktop, output);
		if (roots_output == NULL) {
			return;
		}

		struct wlr_box view_box;
		view_get_box(view, &view_box);

		view->saved.x = view->x;
		view->saved.y = view->y;
		view->saved.rotation = view->rotation;
		view->saved.width = view_box.width;
		view->saved.height = view_box.height;

		struct wlr_box *output_box =
			wlr_output_layout_get_box(view->desktop->layout, output);
		view_move_resize(view, output_box->x, output_box->y, output_box->width,
			output_box->height);
		view_rotate(view, 0);

		roots_output->fullscreen_view = view;
		view->fullscreen_output = roots_output;
		output_damage_whole(roots_output);
	}

	if (was_fullscreen && !fullscreen) {
		view_move_resize(view, view->saved.x, view->saved.y, view->saved.width,
			view->saved.height);
		view_rotate(view, view->saved.rotation);

		output_damage_whole(view->fullscreen_output);
		view->fullscreen_output->fullscreen_view = NULL;
		view->fullscreen_output = NULL;
	}
}

void view_rotate(struct roots_view *view, float rotation) {
	if (view->rotation == rotation) {
		return;
	}

	view_damage_whole(view);
	view->rotation = rotation;
	view_damage_whole(view);
}

void view_close(struct roots_view *view) {
	if (view->close) {
		view->close(view);
	}
}

bool view_center(struct roots_view *view) {
	struct wlr_box box;
	view_get_box(view, &box);

	struct roots_desktop *desktop = view->desktop;
	struct roots_input *input = desktop->server->input;
	struct roots_seat *seat = NULL, *_seat;
	wl_list_for_each(_seat, &input->seats, link) {
		if (!seat || (seat->seat->last_event.tv_sec > _seat->seat->last_event.tv_sec &&
				seat->seat->last_event.tv_nsec > _seat->seat->last_event.tv_nsec)) {
			seat = _seat;
		}
	}
	if (!seat) {
		return false;
	}

	struct wlr_output *output =
		wlr_output_layout_output_at(desktop->layout,
				seat->cursor->cursor->x,
				seat->cursor->cursor->y);
	if (!output) {
		// empty layout
		return false;
	}

	const struct wlr_output_layout_output *l_output =
		wlr_output_layout_get(desktop->layout, output);

	int width, height;
	wlr_output_effective_resolution(output, &width, &height);

	double view_x = (double)(width - box.width) / 2 + l_output->x;
	double view_y = (double)(height - box.height) / 2 + l_output->y;
	view_move(view, view_x, view_y);

	return true;
}

void view_child_finish(struct roots_view_child *child) {
	if (child == NULL) {
		return;
	}
	view_damage_whole(child->view);
	wl_list_remove(&child->link);
	wl_list_remove(&child->commit.link);
	wl_list_remove(&child->new_subsurface.link);
}

static void view_child_handle_commit(struct wl_listener *listener,
		void *data) {
	struct roots_view_child *child = wl_container_of(listener, child, commit);
	view_apply_damage(child->view);
}

static void view_child_handle_new_subsurface(struct wl_listener *listener,
		void *data) {
	struct roots_view_child *child =
		wl_container_of(listener, child, new_subsurface);
	struct wlr_subsurface *wlr_subsurface = data;
	subsurface_create(child->view, wlr_subsurface);
}

void view_child_init(struct roots_view_child *child, struct roots_view *view,
		struct wlr_surface *wlr_surface) {
	assert(child->destroy);
	child->view = view;
	child->wlr_surface = wlr_surface;
	child->commit.notify = view_child_handle_commit;
	wl_signal_add(&wlr_surface->events.commit, &child->commit);
	child->new_subsurface.notify = view_child_handle_new_subsurface;
	wl_signal_add(&wlr_surface->events.new_subsurface, &child->new_subsurface);
	wl_list_insert(&view->children, &child->link);
}

static void subsurface_destroy(struct roots_view_child *child) {
	assert(child->destroy == subsurface_destroy);
	struct roots_subsurface *subsurface = (struct roots_subsurface *)child;
	if (subsurface == NULL) {
		return;
	}
	wl_list_remove(&subsurface->destroy.link);
	view_child_finish(&subsurface->view_child);
	free(subsurface);
}

static void subsurface_handle_destroy(struct wl_listener *listener,
		void *data) {
	struct roots_subsurface *subsurface =
		wl_container_of(listener, subsurface, destroy);
	subsurface_destroy((struct roots_view_child *)subsurface);
}

struct roots_subsurface *subsurface_create(struct roots_view *view,
		struct wlr_subsurface *wlr_subsurface) {
	struct roots_subsurface *subsurface =
		calloc(1, sizeof(struct roots_subsurface));
	if (subsurface == NULL) {
		return NULL;
	}
	subsurface->wlr_subsurface = wlr_subsurface;
	subsurface->view_child.destroy = subsurface_destroy;
	view_child_init(&subsurface->view_child, view, wlr_subsurface->surface);
	subsurface->destroy.notify = subsurface_handle_destroy;
	wl_signal_add(&wlr_subsurface->events.destroy, &subsurface->destroy);
	return subsurface;
}

void view_finish(struct roots_view *view) {
	view_damage_whole(view);
	wl_signal_emit(&view->events.destroy, view);

	wl_list_remove(&view->new_subsurface.link);

	struct roots_view_child *child, *tmp;
	wl_list_for_each_safe(child, tmp, &view->children, link) {
		child->destroy(child);
	}

	if (view->fullscreen_output) {
		view->fullscreen_output->fullscreen_view = NULL;
	}
}

static void view_handle_new_subsurface(struct wl_listener *listener,
		void *data) {
	struct roots_view *view = wl_container_of(listener, view, new_subsurface);
	struct wlr_subsurface *wlr_subsurface = data;
	subsurface_create(view, wlr_subsurface);
}

void view_init(struct roots_view *view, struct roots_desktop *desktop) {
	assert(view->wlr_surface);

	view->desktop = desktop;
	wl_signal_init(&view->events.destroy);
	wl_list_init(&view->children);

	struct wlr_subsurface *subsurface;
	wl_list_for_each(subsurface, &view->wlr_surface->subsurface_list,
			parent_link) {
		subsurface_create(view, subsurface);
	}

	view->new_subsurface.notify = view_handle_new_subsurface;
	wl_signal_add(&view->wlr_surface->events.new_subsurface,
		&view->new_subsurface);

	view_damage_whole(view);
}

void view_setup(struct roots_view *view) {
	struct roots_input *input = view->desktop->server->input;
	// TODO what seat gets focus? the one with the last input event?
	struct roots_seat *seat;
	wl_list_for_each(seat, &input->seats, link) {
		roots_seat_set_focus(seat, view);
	}

	view_center(view);
	view_update_output(view, NULL);
}

void view_apply_damage(struct roots_view *view) {
	struct roots_output *output;
	wl_list_for_each(output, &view->desktop->outputs, link) {
		output_damage_from_view(output, view);
	}
}

void view_damage_whole(struct roots_view *view) {
	struct roots_output *output;
	wl_list_for_each(output, &view->desktop->outputs, link) {
		output_damage_whole_view(output, view);
	}
}

void view_update_position(struct roots_view *view, double x, double y) {
	if (view->x == x && view->y == y) {
		return;
	}

	view_damage_whole(view);
	view->x = x;
	view->y = y;
	view_damage_whole(view);
}

void view_update_size(struct roots_view *view, uint32_t width, uint32_t height) {
	if (view->width == width && view->height == height) {
		return;
	}

	view_damage_whole(view);
	view->width = width;
	view->height = height;
	view_damage_whole(view);
}

static bool view_at(struct roots_view *view, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	if (view->type == ROOTS_WL_SHELL_VIEW &&
			view->wl_shell_surface->state == WLR_WL_SHELL_SURFACE_STATE_POPUP) {
		return false;
	}

	double view_sx = lx - view->x;
	double view_sy = ly - view->y;

	struct wlr_surface_state *state = view->wlr_surface->current;
	struct wlr_box box = {
		.x = 0, .y = 0,
		.width = state->width, .height = state->height,
	};
	if (view->rotation != 0.0) {
		// Coordinates relative to the center of the view
		double ox = view_sx - (double)box.width/2,
			oy = view_sy - (double)box.height/2;
		// Rotated coordinates
		double rx = cos(view->rotation)*ox - sin(view->rotation)*oy,
			ry = cos(view->rotation)*oy + sin(view->rotation)*ox;
		view_sx = rx + (double)box.width/2;
		view_sy = ry + (double)box.height/2;
	}

	if (view->type == ROOTS_XDG_SHELL_V6_VIEW) {
		double popup_sx, popup_sy;
		struct wlr_xdg_surface_v6 *popup =
			wlr_xdg_surface_v6_popup_at(view->xdg_surface_v6,
				view_sx, view_sy, &popup_sx, &popup_sy);

		if (popup) {
			*sx = view_sx - popup_sx;
			*sy = view_sy - popup_sy;
			*surface = popup->surface;
			return true;
		}
	}

	if (view->type == ROOTS_WL_SHELL_VIEW) {
		double popup_sx, popup_sy;
		struct wlr_wl_shell_surface *popup =
			wlr_wl_shell_surface_popup_at(view->wl_shell_surface,
				view_sx, view_sy, &popup_sx, &popup_sy);

		if (popup) {
			*sx = view_sx - popup_sx;
			*sy = view_sy - popup_sy;
			*surface = popup->surface;
			return true;
		}
	}

	double sub_x, sub_y;
	struct wlr_subsurface *subsurface =
		wlr_surface_subsurface_at(view->wlr_surface,
			view_sx, view_sy, &sub_x, &sub_y);
	if (subsurface) {
		*sx = view_sx - sub_x;
		*sy = view_sy - sub_y;
		*surface = subsurface->surface;
		return true;
	}

	if (view_get_deco_part(view, view_sx, view_sy)) {
		*sx = view_sx;
		*sy = view_sy;
		*surface = NULL;
		return true;
	}

	if (wlr_box_contains_point(&box, view_sx, view_sy) &&
			pixman_region32_contains_point(&view->wlr_surface->current->input,
				view_sx, view_sy, NULL)) {
		*sx = view_sx;
		*sy = view_sy;
		*surface = view->wlr_surface;
		return true;
	}

	return false;
}

struct roots_view *desktop_view_at(struct roots_desktop *desktop, double lx,
		double ly, struct wlr_surface **surface, double *sx, double *sy) {
	struct wlr_output *wlr_output =
		wlr_output_layout_output_at(desktop->layout, lx, ly);
	if (wlr_output != NULL) {
		struct roots_output *output =
			desktop_output_from_wlr_output(desktop, wlr_output);
		if (output != NULL && output->fullscreen_view != NULL) {
			if (view_at(output->fullscreen_view, lx, ly, surface, sx, sy)) {
				return output->fullscreen_view;
			} else {
				return NULL;
			}
		}
	}

	struct roots_view *view;
	wl_list_for_each(view, &desktop->views, link) {
		if (view_at(view, lx, ly, surface, sx, sy)) {
			return view;
		}
	}
	return NULL;
}

static void handle_layout_change(struct wl_listener *listener, void *data) {
	struct roots_desktop *desktop =
		wl_container_of(listener, desktop, layout_change);

	struct wlr_output *center_output =
		wlr_output_layout_get_center_output(desktop->layout);
	if (center_output == NULL) {
		return;
	}

	struct wlr_box *center_output_box =
		wlr_output_layout_get_box(desktop->layout, center_output);
	double center_x = center_output_box->x + center_output_box->width/2;
	double center_y = center_output_box->y + center_output_box->height/2;

	struct roots_view *view;
	wl_list_for_each(view, &desktop->views, link) {
		struct wlr_box box;
		view_get_box(view, &box);

		if (wlr_output_layout_intersects(desktop->layout, NULL, &box)) {
			continue;
		}

		view_move(view, center_x - box.width/2, center_y - box.height/2);
	}
}

struct roots_desktop *desktop_create(struct roots_server *server,
		struct roots_config *config) {
	wlr_log(L_DEBUG, "Initializing roots desktop");

	struct roots_desktop *desktop = calloc(1, sizeof(struct roots_desktop));
	if (desktop == NULL) {
		return NULL;
	}

	wl_list_init(&desktop->views);
	wl_list_init(&desktop->outputs);

	desktop->new_output.notify = handle_new_output;
	wl_signal_add(&server->backend->events.new_output, &desktop->new_output);

	desktop->server = server;
	desktop->config = config;

	desktop->layout = wlr_output_layout_create();
	desktop->layout_change.notify = handle_layout_change;
	wl_signal_add(&desktop->layout->events.change, &desktop->layout_change);

	desktop->compositor = wlr_compositor_create(server->wl_display,
		server->renderer);

	desktop->xdg_shell_v6 = wlr_xdg_shell_v6_create(server->wl_display);
	wl_signal_add(&desktop->xdg_shell_v6->events.new_surface,
		&desktop->xdg_shell_v6_surface);
	desktop->xdg_shell_v6_surface.notify = handle_xdg_shell_v6_surface;

	desktop->wl_shell = wlr_wl_shell_create(server->wl_display);
	wl_signal_add(&desktop->wl_shell->events.new_surface,
		&desktop->wl_shell_surface);
	desktop->wl_shell_surface.notify = handle_wl_shell_surface;

#ifdef WLR_HAS_XWAYLAND
	const char *cursor_theme = NULL;
	const char *cursor_default = ROOTS_XCURSOR_DEFAULT;
	struct roots_cursor_config *cc =
		roots_config_get_cursor(config, ROOTS_CONFIG_DEFAULT_SEAT_NAME);
	if (cc != NULL) {
		cursor_theme = cc->theme;
		if (cc->default_image != NULL) {
			cursor_default = cc->default_image;
		}
	}

	desktop->xcursor_manager = wlr_xcursor_manager_create(cursor_theme,
		ROOTS_XCURSOR_SIZE);
	if (desktop->xcursor_manager == NULL) {
		wlr_log(L_ERROR, "Cannot create XCursor manager for theme %s",
			cursor_theme);
		free(desktop);
		return NULL;
	}

	if (config->xwayland) {
		desktop->xwayland = wlr_xwayland_create(server->wl_display,
			desktop->compositor);
		wl_signal_add(&desktop->xwayland->events.new_surface,
			&desktop->xwayland_surface);
		desktop->xwayland_surface.notify = handle_xwayland_surface;

		if (wlr_xcursor_manager_load(desktop->xcursor_manager, 1)) {
			wlr_log(L_ERROR, "Cannot load XWayland XCursor theme");
		}
		struct wlr_xcursor *xcursor = wlr_xcursor_manager_get_xcursor(
			desktop->xcursor_manager, cursor_default, 1);
		if (xcursor != NULL) {
			struct wlr_xcursor_image *image = xcursor->images[0];
			wlr_xwayland_set_cursor(desktop->xwayland, image->buffer,
				image->width, image->width, image->height, image->hotspot_x,
				image->hotspot_y);
		}
	}
#endif

	desktop->gamma_control_manager = wlr_gamma_control_manager_create(
		server->wl_display);
	desktop->screenshooter = wlr_screenshooter_create(server->wl_display);
	desktop->server_decoration_manager =
		wlr_server_decoration_manager_create(server->wl_display);
	wlr_server_decoration_manager_set_default_mode(
		desktop->server_decoration_manager,
		WLR_SERVER_DECORATION_MANAGER_MODE_CLIENT);
	desktop->primary_selection_device_manager =
		wlr_primary_selection_device_manager_create(server->wl_display);
	desktop->idle = wlr_idle_create(server->wl_display);

	return desktop;
}

void desktop_destroy(struct roots_desktop *desktop) {
	// TODO
}

struct roots_output *desktop_output_from_wlr_output(
		struct roots_desktop *desktop, struct wlr_output *wlr_output) {
	struct roots_output *output;
	wl_list_for_each(output, &desktop->outputs, link) {
		if (output->wlr_output == wlr_output) {
			return output;
		}
	}
	return NULL;
}
