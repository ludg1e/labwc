// SPDX-License-Identifier: GPL-2.0-only
/*
 * output.c: labwc output and rendering
 *
 * Copyright (C) 2019-2021 Johan Malm
 * Copyright (C) 2020 The Sway authors
 */

#define _POSIX_C_SOURCE 200809L
#include <strings.h>
#include <wlr/backend/drm.h>
#include <wlr/backend/headless.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_drm_lease_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/region.h>
#include <wlr/util/log.h>
#include "common/macros.h"
#include "common/mem.h"
#include "common/scene-helpers.h"
#include "labwc.h"
#include "layers.h"
#include "node.h"
#include "regions.h"
#include "view.h"
#include "xwayland.h"

static void
output_frame_notify(struct wl_listener *listener, void *data)
{
	/*
	 * This function is called every time an output is ready to display a
	 * frame - which is typically at 60 Hz.
	 */
	struct output *output = wl_container_of(listener, output, frame);
	if (!output_is_usable(output)) {
		return;
	}

	struct wlr_output *wlr_output = output->wlr_output;
	struct server *server = output->server;

	if (output->gamma_lut_changed) {
		struct wlr_output_state pending;
		wlr_output_state_init(&pending);
		if (!wlr_scene_output_build_state(output->scene_output, &pending, NULL)) {
			return;
		}
		output->gamma_lut_changed = false;
		struct wlr_gamma_control_v1 *gamma_control =
			wlr_gamma_control_manager_v1_get_control(
				server->gamma_control_manager_v1, wlr_output);
		if (!wlr_gamma_control_v1_apply(gamma_control, &pending)) {
			wlr_output_state_finish(&pending);
			return;
		}

		if (!wlr_output_commit_state(output->wlr_output, &pending)) {
			wlr_gamma_control_v1_send_failed_and_destroy(gamma_control);
			wlr_output_state_finish(&pending);
			return;
		}

		wlr_damage_ring_rotate(&output->scene_output->damage_ring);
		wlr_output_state_finish(&pending);
		return;
	}

	wlr_scene_output_commit(output->scene_output, NULL);

	struct timespec now = { 0 };
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(output->scene_output, &now);
}

static void
output_destroy_notify(struct wl_listener *listener, void *data)
{
	struct output *output = wl_container_of(listener, output, destroy);
	regions_evacuate_output(output);
	regions_destroy(&output->server->seat, &output->regions);
	wl_list_remove(&output->link);
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->request_state.link);

	for (size_t i = 0; i < ARRAY_SIZE(output->layer_tree); i++) {
		wlr_scene_node_destroy(&output->layer_tree[i]->node);
	}
	wlr_scene_node_destroy(&output->layer_popup_tree->node);
	wlr_scene_node_destroy(&output->osd_tree->node);
	wlr_scene_node_destroy(&output->session_lock_tree->node);
	if (output->workspace_osd) {
		wlr_scene_node_destroy(&output->workspace_osd->node);
		output->workspace_osd = NULL;
	}

	struct view *view;
	struct server *server = output->server;
	wl_list_for_each(view, &server->views, link) {
		if (view->output == output) {
			view_on_output_destroy(view);
		}
	}

	/*
	 * Ensure that we don't accidentally try to dereference
	 * the output pointer in some output event handler like
	 * set_gamma.
	 */
	output->wlr_output->data = NULL;

	/*
	 * output->scene_output (if still around at this point) is
	 * destroyed automatically when the wlr_output is destroyed
	 */
	free(output);
}

static void
output_request_state_notify(struct wl_listener *listener, void *data)
{
	/* This ensures nested backends can be resized */
	struct output *output = wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;

	if (!wlr_output_commit_state(output->wlr_output, event->state)) {
		wlr_log(WLR_ERROR, "Backend requested a new state that could not be applied");
	}
}

static void do_output_layout_change(struct server *server);

static bool
can_reuse_mode(struct wlr_output *wlr_output)
{
	return wlr_output->current_mode && wlr_output_test(wlr_output);
}

static void
add_output_to_layout(struct server *server, struct output *output)
{
	struct wlr_output *wlr_output = output->wlr_output;
	struct wlr_output_layout_output *layout_output =
		wlr_output_layout_add_auto(server->output_layout, wlr_output);
	if (!layout_output) {
		wlr_log(WLR_ERROR, "unable to add output to layout");
		return;
	}

	if (!output->scene_output) {
		output->scene_output =
			wlr_scene_output_create(server->scene, wlr_output);
		if (!output->scene_output) {
			wlr_log(WLR_ERROR, "unable to create scene output");
			return;
		}
		/*
		 * Note: wlr_scene_output_layout_add_output() is not
		 * safe to call twice, so we call it only when initially
		 * creating the scene_output.
		 */
		wlr_scene_output_layout_add_output(server->scene_layout,
			layout_output, output->scene_output);
	}
}

static void
new_output_notify(struct wl_listener *listener, void *data)
{
	/*
	 * This event is rasied by the backend when a new output (aka display
	 * or monitor) becomes available.
	 */
	struct server *server = wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	/* Name virtual output */
	if (wlr_output_is_headless(wlr_output) && server->headless.pending_output_name[0] != '\0') {
		wlr_output_set_name(wlr_output, server->headless.pending_output_name);
		server->headless.pending_output_name[0] = '\0';
	}

	/*
	 * We offer any display as available for lease, some apps like
	 * gamescope, want to take ownership of a display when they can
	 * to use planes and present directly.
	 * This is also useful for debugging the DRM parts of
	 * another compositor.
	 */
	if (server->drm_lease_manager && wlr_output_is_drm(wlr_output)) {
		wlr_drm_lease_v1_manager_offer_output(
			server->drm_lease_manager, wlr_output);
	}

	/*
	 * Don't configure any non-desktop displays, such as VR headsets;
	 */
	if (wlr_output->non_desktop) {
		wlr_log(WLR_DEBUG, "Not configuring non-desktop output");
		return;
	}

	/*
	 * Configures the output created by the backend to use our allocator
	 * and our renderer. Must be done once, before commiting the output
	 */
	if (!wlr_output_init_render(wlr_output, server->allocator,
			server->renderer)) {
		wlr_log(WLR_ERROR, "unable to init output renderer");
		return;
	}

	wlr_log(WLR_DEBUG, "enable output");
	wlr_output_enable(wlr_output, true);

	/*
	 * Try to re-use the existing mode if configured to do so.
	 * Failing that, try to set the preferred mode.
	 */
	struct wlr_output_mode *preferred_mode = NULL;
	if (!rc.reuse_output_mode || !can_reuse_mode(wlr_output)) {
		wlr_log(WLR_DEBUG, "set preferred mode");
		/* The mode is a tuple of (width, height, refresh rate). */
		preferred_mode = wlr_output_preferred_mode(wlr_output);
		wlr_output_set_mode(wlr_output, preferred_mode);
	}

	/*
	 * Sometimes the preferred mode is not available due to hardware
	 * constraints (e.g. GPU or cable bandwidth limitations). In these
	 * cases it's better to fallback to lower modes than to end up with
	 * a black screen. See sway@4cdc4ac6
	 */
	if (!wlr_output_test(wlr_output)) {
		wlr_log(WLR_DEBUG,
			"preferred mode rejected, falling back to another mode");
		struct wlr_output_mode *mode;
		wl_list_for_each(mode, &wlr_output->modes, link) {
			if (mode == preferred_mode) {
				continue;
			}
			wlr_output_set_mode(wlr_output, mode);
			if (wlr_output_test(wlr_output)) {
				break;
			}
		}
	}

	if (rc.adaptive_sync) {
		wlr_output_enable_adaptive_sync(wlr_output, true);
		if (!wlr_output_test(wlr_output)) {
			wlr_output_enable_adaptive_sync(wlr_output, false);
			wlr_log(WLR_DEBUG,
				"failed to enable adaptive sync for output %s", wlr_output->name);
		} else {
			wlr_log(WLR_INFO, "adaptive sync enabled for output %s", wlr_output->name);
		}
	}

	wlr_output_commit(wlr_output);

	struct output *output = znew(*output);
	output->wlr_output = wlr_output;
	wlr_output->data = output;
	output->server = server;
	wlr_output_effective_resolution(wlr_output,
		&output->usable_area.width, &output->usable_area.height);
	wl_list_insert(&server->outputs, &output->link);

	output->destroy.notify = output_destroy_notify;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);
	output->frame.notify = output_frame_notify;
	wl_signal_add(&wlr_output->events.frame, &output->frame);

	output->request_state.notify = output_request_state_notify;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);

	wl_list_init(&output->regions);

	/*
	 * Create layer-trees (background, bottom, top and overlay) and
	 * a layer-popup-tree.
	 */
	for (size_t i = 0; i < ARRAY_SIZE(output->layer_tree); i++) {
		output->layer_tree[i] =
			wlr_scene_tree_create(&server->scene->tree);
		node_descriptor_create(&output->layer_tree[i]->node,
			LAB_NODE_DESC_TREE, NULL);
	}
	output->layer_popup_tree = wlr_scene_tree_create(&server->scene->tree);
	node_descriptor_create(&output->layer_popup_tree->node,
		LAB_NODE_DESC_TREE, NULL);
	output->osd_tree = wlr_scene_tree_create(&server->scene->tree);
	node_descriptor_create(&output->osd_tree->node,
		LAB_NODE_DESC_TREE, NULL);
	output->session_lock_tree = wlr_scene_tree_create(&server->scene->tree);
	node_descriptor_create(&output->session_lock_tree->node,
		LAB_NODE_DESC_TREE, NULL);

	/*
	 * Set the z-positions to achieve the following order (from top to
	 * bottom):
	 *	- session lock layer
	 *	- layer-shell popups
	 *	- overlay layer
	 *	- top layer
	 *	- views
	 *	- bottom layer
	 *	- background layer
	 */
	wlr_scene_node_lower_to_bottom(&output->layer_tree[1]->node);
	wlr_scene_node_lower_to_bottom(&output->layer_tree[0]->node);
	wlr_scene_node_raise_to_top(&output->layer_tree[2]->node);
	wlr_scene_node_raise_to_top(&output->layer_tree[3]->node);
	wlr_scene_node_raise_to_top(&output->layer_popup_tree->node);
	wlr_scene_node_raise_to_top(&output->session_lock_tree->node);

	/*
	 * Wait until wlr_output_layout_add_auto() returns before
	 * calling do_output_layout_change(); this ensures that the
	 * wlr_output_cursor is created for the new output.
	 */
	server->pending_output_layout_change++;

	add_output_to_layout(server, output);

	/* Create regions from config */
	regions_reconfigure_output(output);

	if (server->session_lock) {
		session_lock_output_create(server->session_lock, output);
	}

	server->pending_output_layout_change--;
	do_output_layout_change(server);
}

void
output_init(struct server *server)
{
	server->gamma_control_manager_v1 =
		wlr_gamma_control_manager_v1_create(server->wl_display);

	server->new_output.notify = new_output_notify;
	wl_signal_add(&server->backend->events.new_output, &server->new_output);

	/*
	 * Create an output layout, which is a wlroots utility for working with
	 * an arrangement of screens in a physical layout.
	 */
	server->output_layout = wlr_output_layout_create();
	if (!server->output_layout) {
		wlr_log(WLR_ERROR, "unable to create output layout");
		exit(EXIT_FAILURE);
	}
	server->scene_layout = wlr_scene_attach_output_layout(server->scene,
		server->output_layout);
	if (!server->scene_layout) {
		wlr_log(WLR_ERROR, "unable to create scene layout");
		exit(EXIT_FAILURE);
	}

	/* Enable screen recording with wf-recorder */
	wlr_xdg_output_manager_v1_create(server->wl_display,
		server->output_layout);

	wl_list_init(&server->outputs);

	output_manager_init(server);
}

static void
output_update_for_layout_change(struct server *server)
{
	output_update_all_usable_areas(server, /*layout_changed*/ true);
	session_lock_update_for_layout_change();

	/*
	 * "Move" each wlr_output_cursor (in per-output coordinates) to
	 * align with the seat cursor. Re-set the cursor image so that
	 * the cursor isn't invisible on new outputs.
	 */
	wlr_cursor_move(server->seat.cursor, NULL, 0, 0);
	cursor_update_image(&server->seat);
}

static void
output_config_apply(struct server *server,
		struct wlr_output_configuration_v1 *config)
{
	server->pending_output_layout_change++;

	struct wlr_output_configuration_head_v1 *head;
	wl_list_for_each(head, &config->heads, link) {
		struct wlr_output *o = head->state.output;
		struct output *output = output_from_wlr_output(server, o);
		bool output_enabled = head->state.enabled && !output->leased;
		bool need_to_add = output_enabled && !o->enabled;
		bool need_to_remove = !output_enabled && o->enabled;

		wlr_output_enable(o, output_enabled);
		if (output_enabled) {
			/* Output specific actions only */
			if (head->state.mode) {
				wlr_output_set_mode(o, head->state.mode);
			} else {
				int32_t width = head->state.custom_mode.width;
				int32_t height = head->state.custom_mode.height;
				int32_t refresh = head->state.custom_mode.refresh;
				wlr_output_set_custom_mode(o, width,
					height, refresh);
			}
			wlr_output_set_scale(o, head->state.scale);
			wlr_output_set_transform(o, head->state.transform);
			wlr_output_enable_adaptive_sync(o, head->state.adaptive_sync_enabled);
		}
		if (!wlr_output_commit(o)) {
			wlr_log(WLR_ERROR, "Output config commit failed");
			continue;
		}

		/* Only do Layout specific actions if the commit went trough */
		if (need_to_add) {
			add_output_to_layout(server, output);
		}

		if (output_enabled) {
			struct wlr_box pos = {0};
			wlr_output_layout_get_box(server->output_layout, o, &pos);
			if (pos.x != head->state.x || pos.y != head->state.y) {
				/*
				 * This overrides the automatic layout
				 *
				 * wlr_output_layout_add() in fact means _move()
				 */
				wlr_output_layout_add(server->output_layout, o,
					head->state.x, head->state.y);
			}
		}

		if (need_to_remove) {
			regions_evacuate_output(output);
			/*
			 * At time of writing, wlr_output_layout_remove()
			 * indirectly destroys the wlr_scene_output, but
			 * this behavior may change in future. To remove
			 * doubt and avoid either a leak or double-free,
			 * explicitly destroy the wlr_scene_output before
			 * calling wlr_output_layout_remove().
			 */
			wlr_scene_output_destroy(output->scene_output);
			wlr_output_layout_remove(server->output_layout, o);
			output->scene_output = NULL;
		}
	}

	server->pending_output_layout_change--;
	do_output_layout_change(server);
}

static bool
verify_output_config_v1(const struct wlr_output_configuration_v1 *config)
{
	/* TODO implement */
	return true;
}

static void
handle_output_manager_apply(struct wl_listener *listener, void *data)
{
	struct server *server =
		wl_container_of(listener, server, output_manager_apply);
	struct wlr_output_configuration_v1 *config = data;

	bool config_is_good = verify_output_config_v1(config);

	if (config_is_good) {
		output_config_apply(server, config);
		wlr_output_configuration_v1_send_succeeded(config);
	} else {
		wlr_output_configuration_v1_send_failed(config);
	}
	wlr_output_configuration_v1_destroy(config);
	struct output *output;
	wl_list_for_each(output, &server->outputs, link) {
		wlr_xcursor_manager_load(server->seat.xcursor_manager,
			output->wlr_output->scale);
	}

	/* Re-set cursor image in case scale changed */
	cursor_update_focus(server);
	cursor_update_image(&server->seat);
}

/*
 * Take the way outputs are currently configured/layed out and turn that into
 * a struct that we send to clients via the wlr_output_configuration v1
 * interface
 */
static struct
wlr_output_configuration_v1 *create_output_config(struct server *server)
{
	struct wlr_output_configuration_v1 *config =
		wlr_output_configuration_v1_create();
	if (!config) {
		wlr_log(WLR_ERROR, "wlr_output_configuration_v1_create()");
		return NULL;
	}

	struct output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct wlr_output_configuration_head_v1 *head =
			wlr_output_configuration_head_v1_create(config,
				output->wlr_output);
		if (!head) {
			wlr_log(WLR_ERROR,
				"wlr_output_configuration_head_v1_create()");
			wlr_output_configuration_v1_destroy(config);
			return NULL;
		}
		struct wlr_box box;
		wlr_output_layout_get_box(server->output_layout,
			output->wlr_output, &box);
		if (!wlr_box_empty(&box)) {
			head->state.x = box.x;
			head->state.y = box.y;
		} else {
			wlr_log(WLR_ERROR, "failed to get output layout box");
		}
	}
	return config;
}

static void
do_output_layout_change(struct server *server)
{
	if (!server->pending_output_layout_change) {
		struct wlr_output_configuration_v1 *config =
			create_output_config(server);
		if (config) {
			wlr_output_manager_v1_set_configuration(
				server->output_manager, config);
		} else {
			wlr_log(WLR_ERROR,
				"wlr_output_manager_v1_set_configuration()");
		}
		output_update_for_layout_change(server);
	}
}

static void
handle_output_layout_change(struct wl_listener *listener, void *data)
{
	struct server *server =
		wl_container_of(listener, server, output_layout_change);
	do_output_layout_change(server);
}

static void
handle_gamma_control_set_gamma(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server, gamma_control_set_gamma);
	const struct wlr_gamma_control_manager_v1_set_gamma_event *event = data;

	struct output *output = event->output->data;
	if (!output_is_usable(output)) {
		return;
	}
	output->gamma_lut_changed = true;
	wlr_output_schedule_frame(output->wlr_output);
}

void
output_manager_init(struct server *server)
{
	server->output_manager = wlr_output_manager_v1_create(server->wl_display);

	server->output_layout_change.notify = handle_output_layout_change;
	wl_signal_add(&server->output_layout->events.change,
		&server->output_layout_change);

	server->output_manager_apply.notify = handle_output_manager_apply;
	wl_signal_add(&server->output_manager->events.apply,
		&server->output_manager_apply);

	server->gamma_control_set_gamma.notify = handle_gamma_control_set_gamma;
	wl_signal_add(&server->gamma_control_manager_v1->events.set_gamma,
		&server->gamma_control_set_gamma);
}

struct output *
output_from_wlr_output(struct server *server, struct wlr_output *wlr_output)
{
	struct output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (output->wlr_output == wlr_output) {
			return output;
		}
	}
	return NULL;
}

struct output *
output_from_name(struct server *server, const char *name)
{
	struct output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (!output_is_usable(output) || !output->wlr_output->name) {
			continue;
		}
		if (!strcasecmp(name, output->wlr_output->name)) {
			return output;
		}
	}
	return NULL;
}

struct output *
output_nearest_to(struct server *server, int lx, int ly)
{
	double closest_x, closest_y;
	wlr_output_layout_closest_point(server->output_layout, NULL, lx, ly,
		&closest_x, &closest_y);

	return output_from_wlr_output(server,
		wlr_output_layout_output_at(server->output_layout,
			closest_x, closest_y));
}

struct output *
output_nearest_to_cursor(struct server *server)
{
	return output_nearest_to(server, server->seat.cursor->x,
		server->seat.cursor->y);
}

bool
output_is_usable(struct output *output)
{
	/* output_is_usable(NULL) is safe and returns false */
	return output && output->wlr_output->enabled && !output->leased;
}

/* returns true if usable area changed */
static bool
update_usable_area(struct output *output)
{
	struct wlr_box old = output->usable_area;
	layers_arrange(output);

#if HAVE_XWAYLAND
	struct view *view;
	wl_list_for_each(view, &output->server->views, link) {
		if (view->mapped && view->type == LAB_XWAYLAND_VIEW) {
			xwayland_adjust_usable_area(view,
				output->server->output_layout,
				output->wlr_output, &output->usable_area);
		}
	}
#endif
	return !wlr_box_equal(&old, &output->usable_area);
}

void
output_update_usable_area(struct output *output)
{
	if (update_usable_area(output)) {
		regions_update_geometry(output);
#if HAVE_XWAYLAND
		xwayland_update_workarea(output->server);
#endif
		desktop_arrange_all_views(output->server);
	}
}

void
output_update_all_usable_areas(struct server *server, bool layout_changed)
{
	bool usable_area_changed = false;
	struct output *output;

	wl_list_for_each(output, &server->outputs, link) {
		if (update_usable_area(output)) {
			usable_area_changed = true;
			regions_update_geometry(output);
		} else if (layout_changed) {
			regions_update_geometry(output);
		}
	}
	if (usable_area_changed || layout_changed) {
#if HAVE_XWAYLAND
		xwayland_update_workarea(server);
#endif
		desktop_arrange_all_views(server);
	}
}

struct wlr_box
output_usable_area_in_layout_coords(struct output *output)
{
	if (!output) {
		return (struct wlr_box){0};
	}
	struct wlr_box box = output->usable_area;
	double ox = 0, oy = 0;
	wlr_output_layout_output_coords(output->server->output_layout,
		output->wlr_output, &ox, &oy);
	box.x -= ox;
	box.y -= oy;
	return box;
}

struct wlr_box
output_usable_area_scaled(struct output *output)
{
	if (!output) {
		return (struct wlr_box){0};
	}
	struct wlr_box usable = output_usable_area_in_layout_coords(output);
	if (usable.height == output->wlr_output->height
			&& output->wlr_output->scale != 1) {
		usable.height /= output->wlr_output->scale;
	}
	if (usable.width == output->wlr_output->width
			&& output->wlr_output->scale != 1) {
		usable.width /= output->wlr_output->scale;
	}
	return usable;
}

void
handle_output_power_manager_set_mode(struct wl_listener *listener, void *data)
{
	struct server *server = wl_container_of(listener, server,
		output_power_manager_set_mode);
	struct wlr_output_power_v1_set_mode_event *event = data;

	switch (event->mode) {
	case ZWLR_OUTPUT_POWER_V1_MODE_OFF:
		wlr_output_enable(event->output, false);
		wlr_output_commit(event->output);
		break;
	case ZWLR_OUTPUT_POWER_V1_MODE_ON:
		wlr_output_enable(event->output, true);
		if (!wlr_output_test(event->output)) {
			wlr_output_rollback(event->output);
		}
		wlr_output_commit(event->output);
		/*
		 * Re-set the cursor image so that the cursor
		 * isn't invisible on the newly enabled output.
		 */
		cursor_update_image(&server->seat);
		break;
	}
}

void
output_add_virtual(struct server *server, const char *output_name)
{
	if (output_name) {
		/* Prevent creating outputs with the same name */
		struct output *output;
		wl_list_for_each(output, &server->outputs, link) {
			if (wlr_output_is_headless(output->wlr_output) &&
					!strcmp(output->wlr_output->name, output_name)) {
				wlr_log(WLR_DEBUG,
					"refusing to create virtual output with duplicate name");
				return;
			}
		}
		snprintf(server->headless.pending_output_name,
			sizeof(server->headless.pending_output_name), "%s", output_name);
	} else {
		server->headless.pending_output_name[0] = '\0';
	}
	/*
	 * Setting it to (0, 0) here disallows changing resolution from tools like
	 * wlr-randr (returns error)
	 */
	wlr_headless_add_output(server->headless.backend, 1920, 1080);
}

void
output_remove_virtual(struct server *server, const char *output_name)
{
	struct output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (wlr_output_is_headless(output->wlr_output)) {
			if (output_name) {
				/*
				 * Given virtual output name, find and destroy virtual output by
				 * that name.
				 */
				if (!strcmp(output->wlr_output->name, output_name)) {
					wlr_output_destroy(output->wlr_output);
					return;
				}
			} else {
				/*
				 * When virtual output name was no supplied by user, simply
				 * destroy the first virtual output found.
				 */
				wlr_output_destroy(output->wlr_output);
				return;
			}
		}
	}
}
