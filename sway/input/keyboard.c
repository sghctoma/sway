#include <assert.h>
#include <limits.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_idle.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include "sway/commands.h"
#include "sway/desktop/transaction.h"
#include "sway/input/input-manager.h"
#include "sway/input/keyboard.h"
#include "sway/input/seat.h"
#include "sway/ipc-server.h"
#include "log.h"

/**
 * Remove all key ids associated to a keycode from the list of pressed keys
 */
static void state_erase_key(struct sway_shortcut_state *state,
		uint32_t keycode) {
	size_t j = 0;
	for (size_t i = 0; i < state->npressed; ++i) {
		if (i > j) {
			state->pressed_keys[j] = state->pressed_keys[i];
			state->pressed_keycodes[j] = state->pressed_keycodes[i];
		}
		if (state->pressed_keycodes[i] != keycode) {
			++j;
		}
	}
	while(state->npressed > j) {
		--state->npressed;
		state->pressed_keys[state->npressed] = 0;
		state->pressed_keycodes[state->npressed] = 0;
	}
}

/**
 * Add a key id (with associated keycode) to the list of pressed keys,
 * if the list is not full.
 */
static void state_add_key(struct sway_shortcut_state *state,
		uint32_t keycode, uint32_t key_id) {
	if (state->npressed >= SWAY_KEYBOARD_PRESSED_KEYS_CAP) {
		return;
	}
	size_t i = 0;
	while (i < state->npressed && state->pressed_keys[i] < key_id) {
		++i;
	}
	size_t j = state->npressed;
	while (j > i) {
		state->pressed_keys[j] = state->pressed_keys[j - 1];
		state->pressed_keycodes[j] = state->pressed_keycodes[j - 1];
		--j;
	}
	state->pressed_keys[i] = key_id;
	state->pressed_keycodes[i] = keycode;
	state->npressed++;
}

/**
 * Update the shortcut model state in response to new input
 */
static void update_shortcut_state(struct sway_shortcut_state *state,
		struct wlr_event_keyboard_key *event, uint32_t new_key,
		uint32_t raw_modifiers) {
	bool last_key_was_a_modifier = raw_modifiers != state->last_raw_modifiers;
	state->last_raw_modifiers = raw_modifiers;

	if (last_key_was_a_modifier && state->last_keycode) {
		// Last pressed key before this one was a modifier
		state_erase_key(state, state->last_keycode);
	}

	if (event->state == WLR_KEY_PRESSED) {
		// Add current key to set; there may be duplicates
		state_add_key(state, event->keycode, new_key);
		state->last_keycode = event->keycode;
	} else {
		state_erase_key(state, event->keycode);
	}
}

/**
 * If one exists, finds a binding which matches the shortcut model state,
 * current modifiers, release state, and locked state.
 */
static void get_active_binding(const struct sway_shortcut_state *state,
		list_t *bindings, struct sway_binding **current_binding,
		uint32_t modifiers, bool release, bool locked) {
	for (int i = 0; i < bindings->length; ++i) {
		struct sway_binding *binding = bindings->items[i];
		bool binding_locked = binding->flags & BINDING_LOCKED;
		bool binding_release = binding->flags & BINDING_RELEASE;

		if (modifiers ^ binding->modifiers ||
				state->npressed != (size_t)binding->keys->length ||
				release != binding_release ||
				locked > binding_locked) {
			continue;
		}

		bool match = true;
		for (size_t j = 0; j < state->npressed; j++) {
			uint32_t key = *(uint32_t *)binding->keys->items[j];
			if (key != state->pressed_keys[j]) {
				match = false;
				break;
			}
		}
		if (!match) {
			continue;
		}

		if (*current_binding && *current_binding != binding) {
			wlr_log(WLR_DEBUG, "encountered duplicate bindings %d and %d",
					(*current_binding)->order, binding->order);
		} else {
			*current_binding = binding;
		}
		return;
	}
}

/**
 * Execute a built-in, hardcoded compositor binding. These are triggered from a
 * single keysym.
 *
 * Returns true if the keysym was handled by a binding and false if the event
 * should be propagated to clients.
 */
static bool keyboard_execute_compositor_binding(struct sway_keyboard *keyboard,
		const xkb_keysym_t *pressed_keysyms, uint32_t modifiers, size_t keysyms_len) {
	for (size_t i = 0; i < keysyms_len; ++i) {
		xkb_keysym_t keysym = pressed_keysyms[i];
		if (keysym >= XKB_KEY_XF86Switch_VT_1 &&
				keysym <= XKB_KEY_XF86Switch_VT_12) {
			if (wlr_backend_is_multi(server.backend)) {
				struct wlr_session *session =
					wlr_backend_get_session(server.backend);
				if (session) {
					unsigned vt = keysym - XKB_KEY_XF86Switch_VT_1 + 1;
					wlr_session_change_vt(session, vt);
				}
			}
			return true;
		}
	}

	return false;
}

/**
 * Get keysyms and modifiers from the keyboard as xkb sees them.
 *
 * This uses the xkb keysyms translation based on pressed modifiers and clears
 * the consumed modifiers from the list of modifiers passed to keybind
 * detection.
 *
 * On US layout, pressing Alt+Shift+2 will trigger Alt+@.
 */
static size_t keyboard_keysyms_translated(struct sway_keyboard *keyboard,
		xkb_keycode_t keycode, const xkb_keysym_t **keysyms,
		uint32_t *modifiers) {
	struct wlr_input_device *device =
		keyboard->seat_device->input_device->wlr_device;
	*modifiers = wlr_keyboard_get_modifiers(device->keyboard);
	xkb_mod_mask_t consumed = xkb_state_key_get_consumed_mods2(
		device->keyboard->xkb_state, keycode, XKB_CONSUMED_MODE_XKB);
	*modifiers = *modifiers & ~consumed;

	return xkb_state_key_get_syms(device->keyboard->xkb_state,
		keycode, keysyms);
}

/**
 * Get keysyms and modifiers from the keyboard as if modifiers didn't change
 * keysyms.
 *
 * This avoids the xkb keysym translation based on modifiers considered pressed
 * in the state.
 *
 * This will trigger keybinds such as Alt+Shift+2.
 */
static size_t keyboard_keysyms_raw(struct sway_keyboard *keyboard,
		xkb_keycode_t keycode, const xkb_keysym_t **keysyms,
		uint32_t *modifiers) {
	struct wlr_input_device *device =
		keyboard->seat_device->input_device->wlr_device;
	*modifiers = wlr_keyboard_get_modifiers(device->keyboard);

	xkb_layout_index_t layout_index = xkb_state_key_get_layout(
		device->keyboard->xkb_state, keycode);
	return xkb_keymap_key_get_syms_by_level(device->keyboard->keymap,
		keycode, layout_index, 0, keysyms);
}

static void handle_keyboard_key(struct wl_listener *listener, void *data) {
	struct sway_keyboard *keyboard =
		wl_container_of(listener, keyboard, keyboard_key);
	struct sway_seat* seat = keyboard->seat_device->sway_seat;
	struct wlr_seat *wlr_seat = seat->wlr_seat;
	struct wlr_input_device *wlr_device =
		keyboard->seat_device->input_device->wlr_device;
	wlr_idle_notify_activity(seat->input->server->idle, wlr_seat);
	struct wlr_event_keyboard_key *event = data;
	bool input_inhibited = seat->exclusive_client != NULL;

	// Identify new keycode, raw keysym(s), and translated keysym(s)
	xkb_keycode_t keycode = event->keycode + 8;

	const xkb_keysym_t *translated_keysyms;
	uint32_t translated_modifiers;
	size_t translated_keysyms_len =
		keyboard_keysyms_translated(keyboard, keycode, &translated_keysyms,
			&translated_modifiers);

	const xkb_keysym_t *raw_keysyms;
	uint32_t raw_modifiers;
	size_t raw_keysyms_len =
		keyboard_keysyms_raw(keyboard, keycode, &raw_keysyms, &raw_modifiers);

	uint32_t code_modifiers = wlr_keyboard_get_modifiers(wlr_device->keyboard);

	// Update shortcut model state
	update_shortcut_state(&keyboard->state_keycodes, event,
			(uint32_t)keycode, code_modifiers);
	for (size_t i = 0; i < translated_keysyms_len; ++i) {
		update_shortcut_state(&keyboard->state_keysyms_translated,
				event, (uint32_t)translated_keysyms[i],
				code_modifiers);
	}
	for (size_t i = 0; i < raw_keysyms_len; ++i) {
		update_shortcut_state(&keyboard->state_keysyms_raw,
				event, (uint32_t)raw_keysyms[i],
				code_modifiers);
	}

	bool handled = false;

	// Identify active release binding
	struct sway_binding *binding_released = NULL;
	get_active_binding(&keyboard->state_keycodes,
			config->current_mode->keycode_bindings, &binding_released,
			code_modifiers, true, input_inhibited);
	get_active_binding(&keyboard->state_keysyms_translated,
			config->current_mode->keysym_bindings, &binding_released,
			translated_modifiers, true, input_inhibited);
	get_active_binding(&keyboard->state_keysyms_raw,
			config->current_mode->keysym_bindings, &binding_released,
			raw_modifiers, true, input_inhibited);

	// Execute stored release binding once no longer active
	if (keyboard->held_binding && binding_released != keyboard->held_binding &&
			event->state == WLR_KEY_RELEASED) {
		seat_execute_command(seat, keyboard->held_binding);
		handled = true;
	}
	if (binding_released != keyboard->held_binding) {
		keyboard->held_binding = NULL;
	}
	if (binding_released && event->state == WLR_KEY_PRESSED) {
		keyboard->held_binding = binding_released;
	}

	// Identify and execute active pressed binding
	struct sway_binding *binding = NULL;
	if (event->state == WLR_KEY_PRESSED) {
		get_active_binding(&keyboard->state_keycodes,
				config->current_mode->keycode_bindings, &binding,
				code_modifiers, false, input_inhibited);
		get_active_binding(&keyboard->state_keysyms_translated,
				config->current_mode->keysym_bindings, &binding,
				translated_modifiers, false, input_inhibited);
		get_active_binding(&keyboard->state_keysyms_raw,
				config->current_mode->keysym_bindings, &binding,
				raw_modifiers, false, input_inhibited);

		if (binding) {
			seat_execute_command(seat, binding);
			handled = true;
		}
	}

	// Set up (or clear) keyboard repeat for a pressed binding
	if (binding && wlr_device->keyboard->repeat_info.delay > 0) {
		keyboard->repeat_binding = binding;
		if (wl_event_source_timer_update(keyboard->key_repeat_source,
				wlr_device->keyboard->repeat_info.delay) < 0) {
			wlr_log(WLR_DEBUG, "failed to set key repeat timer");
		}
	} else if (keyboard->repeat_binding) {
		keyboard->repeat_binding = NULL;
		if (wl_event_source_timer_update(keyboard->key_repeat_source, 0) < 0) {
			wlr_log(WLR_DEBUG, "failed to disarm key repeat timer");
		}
	}

	// Compositor bindings
	if (!handled && event->state == WLR_KEY_PRESSED) {
		handled = keyboard_execute_compositor_binding(
				keyboard, translated_keysyms, translated_modifiers,
				translated_keysyms_len);
	}
	if (!handled && event->state == WLR_KEY_PRESSED) {
		handled = keyboard_execute_compositor_binding(
				keyboard, raw_keysyms, raw_modifiers,
				raw_keysyms_len);
	}

	if (!handled || event->state == WLR_KEY_RELEASED) {
		wlr_seat_set_keyboard(wlr_seat, wlr_device);
		wlr_seat_keyboard_notify_key(wlr_seat, event->time_msec,
				event->keycode, event->state);
	}

	transaction_commit_dirty();
}

static int handle_keyboard_repeat(void *data) {
	struct sway_keyboard *keyboard = (struct sway_keyboard *)data;
	struct wlr_keyboard *wlr_device =
			keyboard->seat_device->input_device->wlr_device->keyboard;
	if (keyboard->repeat_binding) {
		if (wlr_device->repeat_info.rate > 0) {
			// We queue the next event first, as the command might cancel it
			if (wl_event_source_timer_update(keyboard->key_repeat_source,
					1000 / wlr_device->repeat_info.rate) < 0) {
				wlr_log(WLR_DEBUG, "failed to update key repeat timer");
			}
		}

		seat_execute_command(keyboard->seat_device->sway_seat,
				keyboard->repeat_binding);
		transaction_commit_dirty();
	}
	return 0;
}

static void determine_bar_visibility(uint32_t modifiers) {
	for (int i = 0; i < config->bars->length; ++i) {
		struct bar_config *bar = config->bars->items[i];
		if (strcmp(bar->mode, bar->hidden_state) == 0) { // both are "hide"
			bool should_be_visible = (~modifiers & bar->modifier) == 0;
			if (bar->visible_by_modifier != should_be_visible) {
				bar->visible_by_modifier = should_be_visible;
				ipc_event_bar_state_update(bar);
			}
		}
	}
}

static void handle_keyboard_modifiers(struct wl_listener *listener,
		void *data) {
	struct sway_keyboard *keyboard =
		wl_container_of(listener, keyboard, keyboard_modifiers);
	struct wlr_seat *wlr_seat = keyboard->seat_device->sway_seat->wlr_seat;
	struct wlr_input_device *wlr_device =
		keyboard->seat_device->input_device->wlr_device;
	wlr_seat_set_keyboard(wlr_seat, wlr_device);
	wlr_seat_keyboard_notify_modifiers(wlr_seat, &wlr_device->keyboard->modifiers);

	uint32_t modifiers = wlr_keyboard_get_modifiers(wlr_device->keyboard);
	determine_bar_visibility(modifiers);
}

struct sway_keyboard *sway_keyboard_create(struct sway_seat *seat,
		struct sway_seat_device *device) {
	struct sway_keyboard *keyboard =
		calloc(1, sizeof(struct sway_keyboard));
	if (!sway_assert(keyboard, "could not allocate sway keyboard")) {
		return NULL;
	}

	keyboard->seat_device = device;
	device->keyboard = keyboard;

	wl_list_init(&keyboard->keyboard_key.link);
	wl_list_init(&keyboard->keyboard_modifiers.link);

	keyboard->key_repeat_source = wl_event_loop_add_timer(server.wl_event_loop,
			handle_keyboard_repeat, keyboard);

	return keyboard;
}

void sway_keyboard_configure(struct sway_keyboard *keyboard) {
	struct xkb_rule_names rules;
	memset(&rules, 0, sizeof(rules));
	struct input_config *input_config =
		input_device_get_config(keyboard->seat_device->input_device);
	struct wlr_input_device *wlr_device =
		keyboard->seat_device->input_device->wlr_device;

	if (input_config && input_config->xkb_layout) {
		rules.layout = input_config->xkb_layout;
	} else {
		rules.layout = getenv("XKB_DEFAULT_LAYOUT");
	}
	if (input_config && input_config->xkb_model) {
		rules.model = input_config->xkb_model;
	} else {
		rules.model = getenv("XKB_DEFAULT_MODEL");
	}

	if (input_config && input_config->xkb_options) {
		rules.options = input_config->xkb_options;
	} else {
		rules.options = getenv("XKB_DEFAULT_OPTIONS");
	}

	if (input_config && input_config->xkb_rules) {
		rules.rules = input_config->xkb_rules;
	} else {
		rules.rules = getenv("XKB_DEFAULT_RULES");
	}

	if (input_config && input_config->xkb_variant) {
		rules.variant = input_config->xkb_variant;
	} else {
		rules.variant = getenv("XKB_DEFAULT_VARIANT");
	}

	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!sway_assert(context, "cannot create XKB context")) {
		return;
	}

	struct xkb_keymap *keymap =
		xkb_keymap_new_from_names(context, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);

	if (!keymap) {
		wlr_log(WLR_DEBUG, "cannot configure keyboard: keymap does not exist");
		xkb_context_unref(context);
		return;
	}

	xkb_keymap_unref(keyboard->keymap);
	keyboard->keymap = keymap;
	wlr_keyboard_set_keymap(wlr_device->keyboard, keyboard->keymap);

	xkb_mod_mask_t locked_mods = 0;
	if (input_config && input_config->xkb_numlock > 0) {
		xkb_mod_index_t mod_index = xkb_map_mod_get_index(keymap, XKB_MOD_NAME_NUM);
		if (mod_index != XKB_MOD_INVALID) {
		       locked_mods |= (uint32_t)1 << mod_index;
		}
	}
	if (input_config && input_config->xkb_capslock > 0) {
		xkb_mod_index_t mod_index = xkb_map_mod_get_index(keymap, XKB_MOD_NAME_CAPS);
		if (mod_index != XKB_MOD_INVALID) {
		       locked_mods |= (uint32_t)1 << mod_index;
		}
	}
	if (locked_mods) {
		wlr_keyboard_notify_modifiers(wlr_device->keyboard, 0, 0, locked_mods, 0);
		uint32_t leds = 0;
		for (uint32_t i = 0; i < WLR_LED_COUNT; ++i) {
			if (xkb_state_led_index_is_active(wlr_device->keyboard->xkb_state,
					wlr_device->keyboard->led_indexes[i])) {
				leds |= (1 << i);
			}
		}
		wlr_keyboard_led_update(wlr_device->keyboard, leds);
	}

	if (input_config && input_config->repeat_delay != INT_MIN
			&& input_config->repeat_rate != INT_MIN) {
		wlr_keyboard_set_repeat_info(wlr_device->keyboard,
				input_config->repeat_rate, input_config->repeat_delay);
	} else {
		wlr_keyboard_set_repeat_info(wlr_device->keyboard, 25, 600);
	}
	xkb_context_unref(context);
	struct wlr_seat *seat = keyboard->seat_device->sway_seat->wlr_seat;
	wlr_seat_set_keyboard(seat, wlr_device);

	wl_list_remove(&keyboard->keyboard_key.link);
	wl_signal_add(&wlr_device->keyboard->events.key, &keyboard->keyboard_key);
	keyboard->keyboard_key.notify = handle_keyboard_key;

	wl_list_remove(&keyboard->keyboard_modifiers.link);
	wl_signal_add(&wlr_device->keyboard->events.modifiers,
		&keyboard->keyboard_modifiers);
	keyboard->keyboard_modifiers.notify = handle_keyboard_modifiers;
}

void sway_keyboard_destroy(struct sway_keyboard *keyboard) {
	if (!keyboard) {
		return;
	}
	if (keyboard->keymap) {
		xkb_keymap_unref(keyboard->keymap);
	}
	wl_list_remove(&keyboard->keyboard_key.link);
	wl_list_remove(&keyboard->keyboard_modifiers.link);
	wl_event_source_remove(keyboard->key_repeat_source);
	free(keyboard);
}
