#include <assert.h>
#include <errno.h>
#include <cairo/cairo.h>
#include <getopt.h>
#include <libinput.h>
#include <libudev.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "devmgr.h"
#include "shm.h"
#include "pango.h"
#include "keymap.h"
#include "config.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

static const size_t COLOR_POOL_COUNT = sizeof(COLOR_POOL) / sizeof(COLOR_POOL[0]);

#define MAX_POOL_COLORS 32
#define DEFAULT_MARGIN 32
#define DEFAULT_MOD_PAD ""

#define IPC_CMD_PAUSE 'P'
#define IPC_CMD_RESUME 'R'
#define IPC_CMD_OPACITY 'O'
#define IPC_CMD_RELOAD 'K'
#define IPC_CMD_MOVE 'M'
#define IPC_CMD_POOL 'p'

static bool pool_enabled = false;
static const char *runtime_pool[MAX_POOL_COLORS];
static size_t runtime_pool_count = 0;
static char *pool_input_buf = nullptr;

#ifdef WSK_DEBUG
static FILE *trace_file = nullptr;
#define WSK_TRACE(fmt, ...)                                                                                            \
	do {                                                                                                           \
		if (trace_file) {                                                                                      \
			struct timespec _ts;                                                                           \
			clock_gettime(CLOCK_MONOTONIC, &_ts);                                                          \
			fprintf(trace_file, "[%ld.%06ld] " fmt "\n", _ts.tv_sec, _ts.tv_nsec / 1000, ##__VA_ARGS__);   \
			fflush(trace_file);                                                                            \
		}                                                                                                      \
	} while (0)
#else
#define WSK_TRACE(fmt, ...)                                                                                            \
	do {                                                                                                           \
	} while (0)
#endif

#define MASK_PATTERNS_MAX 32
#define MASK_BUFFER_MAX 256
#define MASK_THRESHOLD 3

struct mask_state {
	char patterns[MASK_PATTERNS_MAX][256];
	int num_patterns;
	int pos[MASK_PATTERNS_MAX];
	int matched[MASK_PATTERNS_MAX];
	bool active[MASK_PATTERNS_MAX];
	struct wsk_keypress *buffer[MASK_BUFFER_MAX];
	int buffer_len;
};

struct wsk_keypress {
	xkb_keysym_t sym;
	char name[128];
	char utf8[128];
	bool is_repeat;
	struct wsk_keypress *next;
};

struct wsk_output {
	struct wl_output *output;
	struct zxdg_output_v1 *xdg_output;
	int scale;
	enum wl_output_subpixel subpixel;
	char name[128];
	uint32_t registry_name;
	int32_t x, y, logical_w, logical_h;
	struct wsk_output *next;
};

enum text_align { TEXT_ALIGN_LEFT = 0, TEXT_ALIGN_CENTER, TEXT_ALIGN_RIGHT };

struct wsk_state {
	int devmgr;
	pid_t devmgr_pid;
	struct udev *udev;
	struct libinput *libinput;

	uint32_t foreground, background, specialfg, repeatfg;
	const char *font;
	char *repeat_font;
	int timeout;
	int length_limit;
	int fixed_width; /* 0=dynamic resize per keystroke, >0=fixed logical pixels */
	uint32_t min_height;
	int repeat_threshold;

	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct wl_seat *seat;
	struct wl_keyboard *keyboard;
	struct zxdg_output_manager_v1 *output_mgr;
	struct zwlr_layer_shell_v1 *layer_shell;

	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer_surface;
	uint32_t width, height;
	struct wsk_output *output, *outputs;

	struct xkb_state *xkb_state;
	struct xkb_context *xkb_context;
	struct xkb_keymap *xkb_keymap;

	struct wsk_keypress *keys; //the begin of the output keylink
	struct timespec last_key;

	bool run;
	bool inspect;
	bool last_was_release;
	//state of function key
	int ctrl_l_hold;
	int ctrl_r_hold;
	int alt_l_hold;
	int alt_r_hold;
	int super_l_hold;
	int super_r_hold;
	int shift_l_hold;
	int shift_r_hold;

	char current_combination_key[128];
	char prev_combination_key[128];

	int combination_key_repetition;
	bool key_held;
	struct timespec last_repeat_time;
	char repeat_state; /* 0=idle, 1=delayed (past initial 400ms), 2=active (repeating) */
	bool resize_pending;
	int sock_fd;
	bool paused;
	char sock_path[256];
	bool dirty;
	struct pool_buffer buffer_pool[2];
	cairo_surface_t *recording;
	cairo_t *recording_cairo;
	cairo_font_options_t *font_options;
	bool backspace_delete;
	enum text_align text_align;
	struct mask_state mask;

	enum { OUTPUT_DEFAULT, OUTPUT_PINNED } output_mode;
	char target_output_name[128];
	uint32_t anchor;
	int margin;
	char mod_pad[16];
	float opacity;

	int32_t target_x, target_y, target_w, target_h;
	int32_t target_slurp_w;
	bool has_target_position;
	bool has_explicit_margin;
	bool has_explicit_output;
	bool has_explicit_anchor;
	bool has_center;
	char target_position_arg[256];
};

static int find_modifier(const char *name) {
	static const char *mod_names[] = {
		"Control_L", "Control_R", "Alt_L",   "Meta_L",	"Alt_R",
		"Meta_R",    "Super_L",	  "Super_R", "Shift_L", "Shift_R",
	};
	for (int i = 0; i < 10; i++)
		if (strcmp(name, mod_names[i]) == 0)
			return i;
	return -1;
}

static int *modifier_hold_ptr(struct wsk_state *state, int idx) {
	switch (idx) {
		case 0:
			return &state->ctrl_l_hold;
		case 1:
			return &state->ctrl_r_hold;
		case 2: /* Alt_L */
		case 3: /* Meta_L alias */
			return &state->alt_l_hold;
		case 4: /* Alt_R */
		case 5: /* Meta_R alias */
			return &state->alt_r_hold;
		case 6:
			return &state->super_l_hold;
		case 7:
			return &state->super_r_hold;
		case 8:
			return &state->shift_l_hold;
		case 9:
			return &state->shift_r_hold;
	}
	return nullptr;
}

static const char *modifier_display_name(int idx) {
	static const char *names[] = {
		"Control_L", "Control_R", "Alt_L",   "Alt_L",	"Alt_R",
		"Alt_R",     "Super_L",	  "Super_R", "Shift_L", "Shift_R",
	};
	if (idx >= 0 && idx < 10)
		return names[idx];
	return nullptr;
}

static void *safe_calloc(size_t nmemb, size_t size) {
	void *ptr = calloc(nmemb, size);
	if (!ptr && nmemb && size) {
		fprintf(stderr, "out of memory\n");
		abort();
	}
	return ptr;
}

static void cairo_set_source_u32(cairo_t *cairo, uint32_t color, float opacity) {
	cairo_set_source_rgba(cairo, (color >> (3 * 8) & 0xFF) / 255.0, (color >> (2 * 8) & 0xFF) / 255.0,
			      (color >> (1 * 8) & 0xFF) / 255.0, (color >> (0 * 8) & 0xFF) / 255.0 * opacity);
}

static cairo_subpixel_order_t to_cairo_subpixel_order(enum wl_output_subpixel subpixel) {
	switch (subpixel) {
		case WL_OUTPUT_SUBPIXEL_HORIZONTAL_RGB:
			return CAIRO_SUBPIXEL_ORDER_RGB;
		case WL_OUTPUT_SUBPIXEL_HORIZONTAL_BGR:
			return CAIRO_SUBPIXEL_ORDER_BGR;
		case WL_OUTPUT_SUBPIXEL_VERTICAL_RGB:
			return CAIRO_SUBPIXEL_ORDER_VRGB;
		case WL_OUTPUT_SUBPIXEL_VERTICAL_BGR:
			return CAIRO_SUBPIXEL_ORDER_VBGR;
		default:
			return CAIRO_SUBPIXEL_ORDER_DEFAULT;
	}
	return CAIRO_SUBPIXEL_ORDER_DEFAULT;
}


static uint32_t parse_color(const char *color);

static void apply_pool_colors(const char *colors) {
	free(pool_input_buf);
	runtime_pool_count = 0;
	if (colors) {
		pool_input_buf = strdup(colors);
		char *token = strtok(pool_input_buf, ",");
		while (token && runtime_pool_count < MAX_POOL_COLORS) {
			runtime_pool[runtime_pool_count++] = token;
			token = strtok(nullptr, ",");
		}
	} else {
		pool_input_buf = nullptr;
		for (size_t i = 0; i < COLOR_POOL_COUNT && i < MAX_POOL_COLORS; i++)
			runtime_pool[i] = COLOR_POOL[i];
		runtime_pool_count = COLOR_POOL_COUNT;
	}
}

static inline uint32_t get_pool_color(size_t position, uint32_t fallback) {
	if (!pool_enabled || runtime_pool_count == 0)
		return fallback;
	return parse_color(runtime_pool[position % runtime_pool_count]);
}

static const KeymapEntry *keymap_entry(const char *name) {
	const KeymapEntry *e = config_lookup(name);
	if (e)
		return e;

	for (size_t i = 0; i < KEYMAP_LEN; i++)
		if (!strcmp(keymap[i].name, name))
			return &keymap[i];

	size_t len = strlen(name);
	if (len > 2 && name[len - 2] == '_' && (name[len - 1] == 'L' || name[len - 1] == 'R')) {
		char base[128];
		size_t base_len = len - 2;
		memcpy(base, name, base_len);
		base[base_len] = '\0';

		e = config_lookup(base);
		if (e)
			return e;

		for (size_t i = 0; i < KEYMAP_LEN; i++)
			if (!strcmp(keymap[i].name, base))
				return &keymap[i];
	}
	return nullptr;
}

static const char *keypress_display(struct wsk_state *state, struct wsk_keypress *key) {
	if (state->inspect)
		return key->name;
	const KeymapEntry *entry = keymap_entry(key->name);
	if (entry && entry->display)
		return entry->display;
	if (key->utf8[0])
		return key->utf8;
	return key->name;
}

/* Padding inserted before a key. Modifier keys are followed by the key they
 * modify — give that transition an explicit gap (state->mod_pad, -M flag). */
static const char *key_pad_before(const struct wsk_state *state, const char *prev_display, bool prev_is_mod,
				  const char *name) {
	size_t prev_len = prev_display ? strlen(prev_display) : 0;
	if (prev_len > 0 && prev_display[prev_len - 1] == '+')
		return "";
	if (prev_is_mod && find_modifier(name) < 0)
		return state->mod_pad;
	return KEY_PAD_BEFORE;
}

//show key in keylink(begin at state->keys)
static void render_to_cairo(cairo_t *cairo, struct wsk_state *state, int scale, uint32_t *width, uint32_t *height) {
	cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_u32(cairo, state->background, state->opacity);
	cairo_paint(cairo);

	/* Use font metrics for stable height — avoids per-glyph shifting */
	int max_h = get_font_line_height(cairo, state->font, scale);
	if (state->repeat_font) {
		int repeat_h = get_font_line_height(cairo, state->repeat_font, scale);
		if (repeat_h > max_h)
			max_h = repeat_h;
	}

	/* Second pass: draw keys with vertical alignment offset */
	struct wsk_keypress *key = state->keys;
	const char *prev_display = nullptr;
	bool prev_is_mod = false;
	size_t position = 0;
	while (key) {
		const char *display = keypress_display(state, key);
		bool is_mod = find_modifier(key->name) >= 0;
		uint32_t color;

		const KeymapEntry *entry = keymap_entry(key->name);
		if (state->inspect) {
			color = state->specialfg;
		} else if (entry) {
			if (entry->fg && !POOL_OVERRIDES_FG) {
				color = parse_color(entry->fg);
			} else {
				color = get_pool_color(position,
						       entry->fg ? parse_color(entry->fg) : state->foreground);
			}
		} else if (key->utf8[0]) {
			color = get_pool_color(position, state->foreground);
		} else {
			color = get_pool_color(position, state->foreground);
		}

		if (key->is_repeat) {
			color = state->repeatfg;
		}

		const char *pad_before = key_pad_before(state, prev_display, prev_is_mod, key->name);

		const char *use_font = key->is_repeat ? state->repeat_font : state->font;
		int w, h;
		get_text_size(cairo, use_font, &w, &h, nullptr, scale, "%s%s%s", pad_before, display, KEY_PAD_AFTER);

		int target_h = max_h + (int) state->min_height;

		int y_offset = 0;
#if defined(TEXT_ALIGN_CENTER)
		y_offset = (target_h - h) / 2;
#elif defined(TEXT_ALIGN_BOTTOM)
		y_offset = target_h - h;
#endif

		cairo_set_source_u32(cairo, color, state->opacity);
		cairo_move_to(cairo, *width, y_offset);
		pango_printf(cairo, use_font, scale, "%s%s%s", pad_before, display, KEY_PAD_AFTER);

		*width += w;
		if ((int) *height < target_h)
			*height = target_h;
		prev_display = display;
		key = key->next;
		position++;
		prev_is_mod = is_mod;
	}
}

static int compute_fixed_width(struct wsk_state *state) {
	cairo_surface_t *tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	cairo_t *cr = cairo_create(tmp);
	int n = state->length_limit;
	char *buf = malloc(n + 1);
	memset(buf, 'W', n);
	buf[n] = '\0';
	int w = 0, h = 0;
	get_text_size(cr, state->font, &w, &h, nullptr, 1.0, "%s", buf);
	free(buf);
	cairo_destroy(cr);
	cairo_surface_destroy(tmp);
	if (state->fixed_width < 0)
		return w;
	int fixed = state->fixed_width;
	if (w > fixed)
		fixed = w;
	return fixed;
}

static void commit_buffer(struct wsk_state *state, int scale, uint32_t buf_w, uint32_t buf_h, bool paint_content,
			  int x_offset) {
	struct pool_buffer *buf = get_next_buffer(state->shm, state->buffer_pool, buf_w * scale, buf_h * scale);
	if (!buf)
		return;
	cairo_t *shm = buf->cairo;
	cairo_save(shm);
	cairo_set_operator(shm, CAIRO_OPERATOR_CLEAR);
	cairo_paint(shm);
	cairo_restore(shm);
	if (paint_content) {
		cairo_set_source_surface(shm, state->recording, (double) x_offset, 0.0);
		cairo_paint(shm);
	}
	wl_surface_set_buffer_scale(state->surface, scale);
	wl_surface_attach(state->surface, buf->buffer, 0, 0);
	wl_surface_damage_buffer(state->surface, 0, 0, buf_w * scale, buf_h * scale);
	wl_surface_commit(state->surface);
}

static void render_frame_dynamic(struct wsk_state *state, int scale, uint32_t width, uint32_t height);
static void render_frame_fixed(struct wsk_state *state, int scale, uint32_t width, uint32_t height);

static void render_frame(struct wsk_state *state) {
	WSK_TRACE("render_frame entry");
	int scale = state->output ? state->output->scale : 1;

	if (!state->recording) {
		state->recording = cairo_recording_surface_create(CAIRO_CONTENT_COLOR_ALPHA, nullptr);
		state->recording_cairo = cairo_create(state->recording);

		state->font_options = cairo_font_options_create();
		cairo_font_options_set_hint_style(state->font_options, CAIRO_HINT_STYLE_FULL);
		cairo_font_options_set_antialias(state->font_options, CAIRO_ANTIALIAS_SUBPIXEL);
		if (state->output) {
			cairo_font_options_set_subpixel_order(state->font_options,
							      to_cairo_subpixel_order(state->output->subpixel));
		}
	}

	cairo_t *cairo = state->recording_cairo;
	cairo_set_antialias(cairo, CAIRO_ANTIALIAS_BEST);
	cairo_set_font_options(cairo, state->font_options);

	cairo_save(cairo);
	cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cairo);
	cairo_restore(cairo);

	uint32_t width = 0, height = 0;
	render_to_cairo(cairo, state, scale, &width, &height);

	if (state->fixed_width)
		render_frame_fixed(state, scale, width, height);
	else
		render_frame_dynamic(state, scale, width, height);
}

static void clear_buffer_if_configured(struct wsk_state *state, int scale) {
	if (state->width && state->height)
		commit_buffer(state, scale, state->width, state->height, false, 0);
	state->resize_pending = false;
}

static void render_frame_dynamic(struct wsk_state *state, int scale, uint32_t width, uint32_t height) {
	if (height / scale != state->height || width / scale != state->width || state->width == 0) {
		if (width == 0 || height == 0) {
			clear_buffer_if_configured(state, scale);
		} else {
			if (!state->resize_pending) {
				state->resize_pending = true;
				zwlr_layer_surface_v1_set_size(state->layer_surface, width / scale, height / scale);
			}
			if (state->width && state->height)
				commit_buffer(state, scale, state->width, state->height, true, 0);
		}
	} else if (height > 0) {
		commit_buffer(state, scale, state->width, state->height, true, 0);
	}
}

static void render_frame_fixed(struct wsk_state *state, int scale, uint32_t width, uint32_t height) {
	if (height == 0) {
		clear_buffer_if_configured(state, scale);
		return;
	}
	uint32_t target_h = height / scale;
	if (state->height != target_h || state->width != (uint32_t) state->fixed_width) {
		if (!state->resize_pending) {
			state->resize_pending = true;
			zwlr_layer_surface_v1_set_size(state->layer_surface, state->fixed_width, target_h);
		}
		if (state->width && state->height)
			commit_buffer(state, scale, state->width, state->height, true, 0);
	} else {
		int x_off = 0;
		if (state->text_align == TEXT_ALIGN_RIGHT) {
			x_off = state->fixed_width * scale - (int) width;
		} else if (state->text_align == TEXT_ALIGN_CENTER) {
			x_off = (state->fixed_width * scale - (int) width) / 2;
		}
		commit_buffer(state, scale, state->fixed_width, state->height, true, x_off);
	}
}

bool surface_is_configured(struct wsk_state *state) {
	return (state->width && state->height);
}

static void set_dirty(struct wsk_state *state) {
	WSK_TRACE("set_dirty called (was %d)", state->dirty);
	state->dirty = true;
}

#ifdef WSK_DEBUG
static void debug_print_display(struct wsk_state *state) {
	fprintf(trace_file, "[display] ");
	struct wsk_keypress *key = state->keys;
	if (!key) {
		fprintf(trace_file, "(empty)");
		fflush(trace_file);
		return;
	}
	int count = 0;
	while (key) {
		const char *display = keypress_display(state, key);
		if (key->is_repeat) {
			fprintf(trace_file, "‹%s›", display);
		} else {
			fprintf(trace_file, "%s", display);
		}
		count++;
		key = key->next;
	}
	fprintf(trace_file, "  [%d nodes]", count);
	fflush(trace_file);
}
#endif

static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *zwlr_layer_surface_v1, uint32_t serial,
				    uint32_t width, uint32_t height) {
	struct wsk_state *state = data;
	WSK_TRACE("configure serial=%u w=%u h=%u (current w=%u h=%u resize_pending=%d)", serial, width, height,
		  state->width, state->height, state->resize_pending);
	state->width = width;
	state->height = height;
	state->resize_pending = false;
	set_dirty(state);
	zwlr_layer_surface_v1_ack_configure(zwlr_layer_surface_v1, serial);
}

static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *zwlr_layer_surface_v1) {
	struct wsk_state *state = data;
	state->run = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

static void surface_enter(void *data, struct wl_surface *wl_surface, struct wl_output *output) {
	struct wsk_state *state = data;
	struct wsk_output *wsk_output = state->outputs;
	while (wsk_output) {
		if (wsk_output->output == output) {
			state->output = wsk_output;
			return;
		}
		wsk_output = wsk_output->next;
	}
}

static void surface_leave(void *data, struct wl_surface *wl_surface, struct wl_output *output) {
	// Who cares (not really possible with layer shell)
}

static const struct wl_surface_listener wl_surface_listener = {
	.enter = surface_enter,
	.leave = surface_leave,
};

static void keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard, uint32_t format, int32_t fd, uint32_t size) {
	struct wsk_state *state = data;
	char *map_shm = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
	if (map_shm == MAP_FAILED) {
		close(fd);
		fprintf(stderr, "Unable to mmap keymap: %s", strerror(errno));
		return;
	}
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		munmap(map_shm, size);
		close(fd);
		return;
	}

	struct xkb_keymap *keymap = xkb_keymap_new_from_string(state->xkb_context, map_shm, XKB_KEYMAP_FORMAT_TEXT_V1,
							       XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map_shm, size);
	close(fd);

	if (!keymap) {
		fprintf(stderr, "Failed to parse XKB keymap from compositor\n");
		return;
	}

	struct xkb_state *xkb_state = xkb_state_new(keymap);
	xkb_keymap_unref(state->xkb_keymap);
	xkb_state_unref(state->xkb_state);
	state->xkb_keymap = keymap;
	state->xkb_state = xkb_state;
}

static void keyboard_enter(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, struct wl_surface *surface,
			   struct wl_array *keys) {
	// Who cares
}

static void keyboard_leave(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, struct wl_surface *surface) {
	// Who cares
}

static void keyboard_key(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, uint32_t time, uint32_t key,
			 uint32_t state) {
	// Who cares
}

static void keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard, uint32_t serial, uint32_t mods_depressed,
			       uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {
	// Who cares
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard, int32_t rate, int32_t delay) {
	// TODO
}

static const struct wl_keyboard_listener wl_keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat_info,
};

static void seat_capabilities(void *data, struct wl_seat *wl_seat, uint32_t capabilities) {
	struct wsk_state *state = data;
	if (state->keyboard) {
		// TODO: support multiple seats
		return;
	}

	if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD)) {
		fprintf(stderr, "wl_seat does not support keyboard");
		state->run = false;
		return;
	}

	state->keyboard = wl_seat_get_keyboard(wl_seat);
	wl_keyboard_add_listener(state->keyboard, &wl_keyboard_listener, state);
}

static void seat_name(void *data, struct wl_seat *wl_seat, const char *name) {
	struct wsk_state *state = data;
	/* TODO: support multiple seats */
	if (libinput_udev_assign_seat(state->libinput, "seat0") != 0) {
		fprintf(stderr, "Failed to assign libinput seat\n");
		state->run = false;
		return;
	}
}

static const struct wl_seat_listener wl_seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

static void output_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y, int32_t physical_width,
			    int32_t physical_height, int32_t subpixel, const char *make, const char *model,
			    int32_t transform) {
	struct wsk_output *output = data;
	output->subpixel = subpixel;
}

static void output_mode(void *data, struct wl_output *wl_output, uint32_t flags, int32_t width, int32_t height,
			int32_t refresh) {
	// Who cares
}

static void output_done(void *data, struct wl_output *wl_output) {
	// Who cares
}

static void output_scale(void *data, struct wl_output *wl_output, int32_t factor) {
	struct wsk_output *output = data;
	output->scale = factor;
}

static const struct wl_output_listener wl_output_listener = {
	.geometry = output_geometry,
	.mode = output_mode,
	.done = output_done,
	.scale = output_scale,
};

static void xdg_output_handle_name(void *data, struct zxdg_output_v1 *xdg_output, const char *name) {
	struct wsk_output *output = data;
	strncpy(output->name, name, sizeof(output->name) - 1);
	output->name[sizeof(output->name) - 1] = '\0';
}

static void xdg_output_handle_logical_position(void *data, struct zxdg_output_v1 *xdg_output, int32_t x, int32_t y) {
	struct wsk_output *output = data;
	output->x = x;
	output->y = y;
}

static void xdg_output_handle_logical_size(void *data, struct zxdg_output_v1 *xdg_output, int32_t width,
					   int32_t height) {
	struct wsk_output *output = data;
	output->logical_w = width;
	output->logical_h = height;
}

static void xdg_output_handle_done(void *data, struct zxdg_output_v1 *xdg_output) {}

static void xdg_output_handle_description(void *data, struct zxdg_output_v1 *xdg_output, const char *description) {}

static const struct zxdg_output_v1_listener xdg_output_listener = {
	.logical_position = xdg_output_handle_logical_position,
	.logical_size = xdg_output_handle_logical_size,
	.done = xdg_output_handle_done,
	.name = xdg_output_handle_name,
	.description = xdg_output_handle_description,
};

//add keyboard event listen
static void registry_global(void *data, struct wl_registry *wl_registry, uint32_t name, const char *interface,
			    uint32_t version) {
	struct wsk_state *state = data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		state->compositor = wl_registry_bind(wl_registry, name, &wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		state->shm = wl_registry_bind(wl_registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		state->seat = wl_registry_bind(wl_registry, name, &wl_seat_interface, 5);
	} else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
		state->output_mgr = wl_registry_bind(wl_registry, name, &zxdg_output_manager_v1_interface, 3);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		state->layer_shell = wl_registry_bind(wl_registry, name, &zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		struct wsk_output *output = safe_calloc(1, sizeof(struct wsk_output));
		output->output = wl_registry_bind(wl_registry, name, &wl_output_interface, 3);
		output->scale = 1;
		output->name[0] = '\0';
		output->xdg_output = nullptr;
		output->registry_name = name;
		struct wsk_output **link = &state->outputs;
		while (*link) {
			link = &(*link)->next;
		}
		*link = output;
		wl_output_add_listener(output->output, &wl_output_listener, output);
		if (state->output_mgr) {
			output->xdg_output = zxdg_output_manager_v1_get_xdg_output(state->output_mgr, output->output);
			zxdg_output_v1_add_listener(output->xdg_output, &xdg_output_listener, output);
		}
	}
}

static void registry_global_remove(void *data, struct wl_registry *wl_registry, uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

static int calculate_charnum_of_int(int num, bool is_del) {
	if (is_del && num == 1)
		return 0;
	int count = 0;
	while (num != 0) {
		num /= 10;
		++count;
	}
	return count + (is_del ? 1 : 0);
}


static void del_last_key(struct wsk_state *state, int n) {
	struct wsk_keypress **temp_keypress;
	while (n > 0) {
		struct wsk_keypress **link = &state->keys;
		while (*link) {
			temp_keypress = &(*link)->next;
			if ((*temp_keypress) == nullptr) {
				free(*link);
				*link = nullptr;
				break;
			} else {
				link = temp_keypress;
			}
		}
		n--;
	}
}

static void strip_repeat_nodes(struct wsk_state *state) {
	struct wsk_keypress **link = &state->keys;
	while (*link) {
		link = &(*link)->next;
	}
	// link now points to the nullptr at the end — walk back and strip trailing repeat nodes
	struct wsk_keypress **tail = link;
	while (tail != &state->keys) {
		// Find the node just before tail
		struct wsk_keypress **prev = &state->keys;
		while (*prev && &(*prev)->next != tail)
			prev = &(*prev)->next;
		if ((*prev)->is_repeat) {
			struct wsk_keypress *to_free = *prev;
			*prev = to_free->next;
			free(to_free);
			tail = prev;
		} else {
			break;
		}
	}
}

static void attach_to_last(struct wsk_state *state, struct wsk_keypress *key) {
	struct wsk_keypress **attach = &state->keys;
	//get the end of the output keylink
	while (*attach) {
		attach = &(*attach)->next;
	}
	*attach = key;
}

static const char *numchar_map[] = {REPEAT_0, REPEAT_1, REPEAT_2, REPEAT_3, REPEAT_4,
				    REPEAT_5, REPEAT_6, REPEAT_7, REPEAT_8, REPEAT_9};

static void change_numchar_to_special(char *target, char numchar) {
	if (numchar >= '0' && numchar <= '9') {
		strcpy(target, numchar_map[numchar - '0']);
	}
}

static void attach_repeat_flag(struct wsk_state *state, int num, int num_len) {
	struct wsk_keypress *repeat_flag = safe_calloc(1, sizeof(struct wsk_keypress));
	strcpy(repeat_flag->name, REPEAT_MARKER);
	repeat_flag->is_repeat = true;
	attach_to_last(state, repeat_flag);

	char *repeat_num_char = safe_calloc(num_len + 1, sizeof(char));
	sprintf(repeat_num_char, "%d", num);

	for (int i = 0; i < num_len; i++) {
		struct wsk_keypress *repeat_num = safe_calloc(1, sizeof(struct wsk_keypress));
		change_numchar_to_special(repeat_num->name, repeat_num_char[i]);
		repeat_num->is_repeat = true;
		attach_to_last(state, repeat_num);
	}

	free(repeat_num_char);
}

/* Generate a synthetic repeat for a held key */
static void generate_held_key_repeat(struct wsk_state *state) {
	int del_charnum = calculate_charnum_of_int(state->combination_key_repetition, true);
	if (state->combination_key_repetition > state->repeat_threshold - 1)
		del_last_key(state, del_charnum);
	state->combination_key_repetition++;
	if (state->combination_key_repetition > state->repeat_threshold - 1) {
		int add_charnum = calculate_charnum_of_int(state->combination_key_repetition, false);
		attach_repeat_flag(state, state->combination_key_repetition, add_charnum);
	}
	set_dirty(state);
}

static int pattern_char_matches(char pchar, struct wsk_keypress *kp) {
	if (pchar == '?')
		return kp->utf8[0] != '\0' ? 1 : 0;
	if (kp->utf8[0] != '\0')
		return tolower((unsigned char) kp->utf8[0]) == tolower((unsigned char) pchar) ? 1 : 0;
	return tolower((unsigned char) kp->name[0]) == tolower((unsigned char) pchar) ? 1 : 0;
}

static void mask_reset(struct mask_state *m) {
	for (int i = 0; i < m->num_patterns; i++) {
		m->pos[i] = 0;
		m->matched[i] = 0;
		m->active[i] = false;
	}
	m->buffer_len = 0;
}

static int mask_check(struct mask_state *m, struct wsk_keypress *kp) {
	if (m->num_patterns == 0)
		return 0;

	bool any_active = false;
	bool any_full = false;

	for (int i = 0; i < m->num_patterns; i++) {
		int plen = (int) strlen(m->patterns[i]);
		if (plen == 0)
			continue;

		int ppos = m->active[i] ? m->pos[i] : 0;

		if (ppos >= plen)
			continue;

		if (pattern_char_matches(m->patterns[i][ppos], kp)) {
			m->active[i] = true;
			m->pos[i] = ppos + 1;
			m->matched[i]++;
			any_active = true;
			if (m->pos[i] >= plen) {
				any_full = true;
			}
		} else {
			m->active[i] = false;
		}
	}

	if (any_full)
		return 2; // DISCARD

	if (!any_active) {
		int max_m = 0;
		for (int i = 0; i < m->num_patterns; i++)
			if (m->matched[i] > max_m)
				max_m = m->matched[i];
		if (max_m >= MASK_THRESHOLD)
			return 2; // DISCARD
		if (max_m > 0)
			return 3; // FLUSH
		return 0; // PASS
	}

	return 1; // BUFFER
}

static void mask_buffer_add(struct mask_state *m, struct wsk_keypress *kp) {
	if (m->buffer_len >= MASK_BUFFER_MAX)
		return;
	m->buffer[m->buffer_len++] = kp;
}

static void mask_handle_backspace(struct mask_state *m) {
	if (m->buffer_len <= 0)
		return;
	free(m->buffer[m->buffer_len - 1]);
	m->buffer_len--;
	for (int p = 0; p < m->num_patterns; p++) {
		if (m->active[p] && m->pos[p] > 0) {
			m->pos[p]--;
			if (m->matched[p] > 0)
				m->matched[p]--;
		}
	}
}

static int append_key_with_modifiers(struct wsk_state *state, struct wsk_keypress *kp) {
	struct wsk_keypress **link = &state->keys;
	while (*link)
		link = &(*link)->next;
	int n = 0;

	int *last_hold = nullptr;
	for (int i = 0; i < 10; i++) {
		int *hp = modifier_hold_ptr(state, i);
		if (*hp && hp != last_hold) {
			struct wsk_keypress *tk = safe_calloc(1, sizeof(struct wsk_keypress));
			strcpy(tk->name, modifier_display_name(i));
			*link = tk;
			link = &(*link)->next;
			n++;
			last_hold = hp;
		}
	}

	*link = kp;
	kp->next = nullptr;
	n++;
	return n;
}

static void mask_flush(struct wsk_state *state, struct wsk_keypress *current_kp) {
	for (int i = 0; i < state->mask.buffer_len; i++)
		append_key_with_modifiers(state, state->mask.buffer[i]);
	append_key_with_modifiers(state, current_kp);
	// Reset repeat detection
	memset(state->prev_combination_key, 0, sizeof(state->prev_combination_key));
	state->combination_key_repetition = 1;
	mask_reset(&state->mask);
}

static void mask_discard(struct wsk_state *state, struct wsk_keypress *current_kp) {
	for (int i = 0; i < state->mask.buffer_len; i++)
		free(state->mask.buffer[i]);
	free(current_kp);
	mask_reset(&state->mask);
}

//listen key keydown and record to keylink
static void reset_input_state(struct wsk_state *state) {
	state->ctrl_l_hold = 0;
	state->ctrl_r_hold = 0;
	state->alt_l_hold = 0;
	state->alt_r_hold = 0;
	state->super_l_hold = 0;
	state->super_r_hold = 0;
	state->shift_l_hold = 0;
	state->shift_r_hold = 0;
	state->key_held = false;
	state->repeat_state = 0;
	state->last_was_release = true;
	state->combination_key_repetition = 1;
	memset(state->prev_combination_key, 0, sizeof(state->prev_combination_key));
	memset(state->current_combination_key, 0, sizeof(state->current_combination_key));
	clock_gettime(CLOCK_MONOTONIC, &state->last_key);
}

static void handle_libinput_event(struct wsk_state *state, struct libinput_event *event) {
	if (!state->xkb_state) {
		return;
	}

	enum libinput_event_type event_type = libinput_event_get_type(event);
	if (event_type != LIBINPUT_EVENT_KEYBOARD_KEY) {
		return;
	}

	struct libinput_event_keyboard *kbevent = libinput_event_get_keyboard_event(event);

	uint32_t keycode = libinput_event_keyboard_get_key(kbevent) + 8;
	enum libinput_key_state key_state = libinput_event_keyboard_get_key_state(kbevent);
	xkb_state_update_key(state->xkb_state, keycode,
			     key_state == LIBINPUT_KEY_STATE_RELEASED ? XKB_KEY_UP : XKB_KEY_DOWN);

	xkb_keysym_t keysym = xkb_state_key_get_one_sym(state->xkb_state, keycode);

	struct wsk_keypress *keypress;
	keypress = safe_calloc(1, sizeof(struct wsk_keypress));
	keypress->sym = keysym;
	xkb_keysym_get_name(keypress->sym, keypress->name, sizeof(keypress->name));
	if (xkb_state_key_get_utf8(state->xkb_state, keycode, keypress->utf8, sizeof(keypress->utf8)) <= 0 ||
	    keypress->utf8[0] <= ' ') {
		keypress->utf8[0] = '\0';
	}

	WSK_TRACE("key_event sym=%u name=%s state=%s", keysym, keypress->name,
		  key_state == LIBINPUT_KEY_STATE_PRESSED ? "pressed" : "released");

	int mod_idx = find_modifier(keypress->name);
	switch (key_state) {
		case LIBINPUT_KEY_STATE_RELEASED:
			//if 'ctrl shift alt super' release, clear it's press state
			if (mod_idx >= 0) {
				*modifier_hold_ptr(state, mod_idx) = 0;
				free(keypress);
				state->last_was_release = 1;
				clock_gettime(CLOCK_MONOTONIC, &state->last_key);
				return;
			}
			free(keypress);
			break;
		case LIBINPUT_KEY_STATE_PRESSED:
			//if 'ctrl shift alt super' press,mark it's press state
			if (mod_idx >= 0) {
				*modifier_hold_ptr(state, mod_idx) = 1;
				free(keypress);
				state->last_was_release = 0;
				clock_gettime(CLOCK_MONOTONIC, &state->last_key);
				return;
			} else {
				// Pattern masking — intercept before display
				int mask_result = mask_check(&state->mask, keypress);

				// Backspace while pattern buffer active — pop last
				if (state->mask.buffer_len > 0 && strcmp(keypress->name, "BackSpace") == 0) {
					mask_handle_backspace(&state->mask);
					free(keypress);
					state->last_was_release = (key_state == LIBINPUT_KEY_STATE_RELEASED);
					clock_gettime(CLOCK_MONOTONIC, &state->last_key);
					return;
				}

				// Backspace-delete: remove last rendered key when mask is not active
				if (state->backspace_delete && state->mask.buffer_len == 0 &&
				    (strcmp(keypress->name, "BackSpace") == 0)) {
					if (state->keys) {
						strip_repeat_nodes(state);
						del_last_key(state, 1);
						memset(state->prev_combination_key, 0,
						       sizeof(state->prev_combination_key));
						state->combination_key_repetition = 0;
						set_dirty(state);
					}
					free(keypress);
					state->last_was_release = (key_state == LIBINPUT_KEY_STATE_RELEASED);
					clock_gettime(CLOCK_MONOTONIC, &state->last_key);
					return;
				}

				// Key repeat while pattern buffer active — flush/discard
				if (state->combination_key_repetition > 1 && state->mask.buffer_len > 0) {
					int max_m = 0;
					for (int i = 0; i < state->mask.num_patterns; i++)
						if (state->mask.matched[i] > max_m)
							max_m = state->mask.matched[i];
					if (max_m >= MASK_THRESHOLD) {
						for (int i = 0; i < state->mask.buffer_len; i++)
							free(state->mask.buffer[i]);
					} else {
						for (int i = 0; i < state->mask.buffer_len; i++)
							append_key_with_modifiers(state, state->mask.buffer[i]);
					}
					mask_reset(&state->mask);
					// Fall through to normal repeat handling
				}

				if (mask_result == 1) {
					mask_buffer_add(&state->mask, keypress);
					state->last_was_release = (key_state == LIBINPUT_KEY_STATE_RELEASED);
					clock_gettime(CLOCK_MONOTONIC, &state->last_key);
					return;
				}
				if (mask_result == 2) {
					mask_discard(state, keypress);
					state->last_was_release = (key_state == LIBINPUT_KEY_STATE_RELEASED);
					clock_gettime(CLOCK_MONOTONIC, &state->last_key);
					return;
				}
				if (mask_result == 3) {
					// Build current_combination_key from the keys we're about to flush
					memset(state->current_combination_key, 0,
					       sizeof(state->current_combination_key));
					mask_flush(state, keypress);
					set_dirty(state);
					state->last_was_release = (key_state == LIBINPUT_KEY_STATE_RELEASED);
					clock_gettime(CLOCK_MONOTONIC, &state->last_key);
					return;
				}

				struct wsk_keypress **link = &state->keys;
				while (*link) {
					link = &(*link)->next;
				}

				memset(state->current_combination_key, 0, sizeof(state->current_combination_key));

				char *combo = state->current_combination_key;
				size_t combo_left = sizeof(state->current_combination_key);
				int n;
				int *last_hold = nullptr;
				for (int i = 0; i < 10; i++) {
					int *hp = modifier_hold_ptr(state, i);
					if (*hp && hp != last_hold && combo_left > 1) {
						n = snprintf(combo, combo_left, "%s", modifier_display_name(i));
						if (n >= (int) combo_left)
							n = (int) combo_left - 1;
						combo += n;
						combo_left -= n;
						last_hold = hp;
					}
				}
				if (combo_left > 1)
					snprintf(combo, combo_left, "%s", keypress->name);

				if (strcmp(state->prev_combination_key, "") != 0 &&
				    strcmp(state->prev_combination_key, state->current_combination_key) == 0) {
					// === REPEAT PATH ===
					if (state->combination_key_repetition < state->repeat_threshold - 1) {
						// 2nd press: add key node (no counter yet)
						*link = keypress;
					} else {
						// 3rd+ press: counter shows, don't add key node
						free(keypress);
						if (state->combination_key_repetition > state->repeat_threshold - 1)
							del_last_key(state,
								     calculate_charnum_of_int(
									     state->combination_key_repetition, true));
					}
					state->combination_key_repetition++;
					if (state->combination_key_repetition > state->repeat_threshold - 1) {
						int add_charnum = calculate_charnum_of_int(
							state->combination_key_repetition, false);
						attach_repeat_flag(state, state->combination_key_repetition,
								   add_charnum);
					}
				} else {
					append_key_with_modifiers(state, keypress);

					snprintf(state->prev_combination_key, sizeof(state->prev_combination_key), "%s",
						 state->current_combination_key);
					state->combination_key_repetition = 1;
				}
			}
			break;
	}

	/* Track physical key hold (for synthetic repeat timer) */
	if (key_state == LIBINPUT_KEY_STATE_PRESSED) {
		state->key_held = true;
	} else if (key_state == LIBINPUT_KEY_STATE_RELEASED) {
		state->key_held = false;
		state->repeat_state = 0;
	}

	state->last_was_release = (key_state == LIBINPUT_KEY_STATE_RELEASED);
	clock_gettime(CLOCK_MONOTONIC, &state->last_key);
	set_dirty(state);
#ifdef WSK_DEBUG
	debug_print_display(state);
#endif
}

static int libinput_open_restricted(const char *path, int flags, void *data) {
	int *fd = data;
	return devmgr_open(*fd, path);
}

static void libinput_close_restricted(int fd, void *data) {
	close(fd);
}

static const struct libinput_interface libinput_impl = {
	.open_restricted = libinput_open_restricted,
	.close_restricted = libinput_close_restricted,
};

static uint32_t parse_color(const char *color) {
	if (color[0] == '#') {
		++color;
	}

	int len = strlen(color);
	if (len != 6 && len != 8) {
		fprintf(stderr,
			"Invalid color %s, defaulting to color "
			"0xFFFFFFFF\n",
			color);
		return 0xFFFFFFFF;
	}
	uint32_t res = (uint32_t) strtoul(color, nullptr, 16);
	if (strlen(color) == 6) {
		res = (res << 8) | 0xFF;
	}
	return res;
}

void clear_full_keylink(struct wsk_keypress *key, struct wsk_state *state) {
	WSK_TRACE("clear_full_keylink");
#ifdef WSK_DEBUG
	printf("\n");
	fflush(stdout);
#endif
	if (!key) {
		return;
	}
	while (key) {
		struct wsk_keypress *next = key->next;
		free(key);
		key = next;
	}
	state->combination_key_repetition = 1;
	memset(state->current_combination_key, 0, sizeof(state->current_combination_key));
	memset(state->prev_combination_key, 0, sizeof(state->prev_combination_key));
	state->keys = nullptr;
	set_dirty(state);
}

static const char *get_sock_path(char *buf, size_t bufsize) {
	const char *dir = getenv("XDG_RUNTIME_DIR");
	if (dir) {
		snprintf(buf, bufsize, "%s/wiv.sock", dir);
	} else {
		snprintf(buf, bufsize, "/tmp/wiv.sock");
	}
	return buf;
}

static void compute_g_margins(uint32_t anchor, bool has_center, int32_t local_x, int32_t local_y, int32_t output_w,
			      int32_t output_h, int32_t surf_w, int32_t surf_h, int32_t *m_top, int32_t *m_right,
			      int32_t *m_bottom, int32_t *m_left) {
	*m_top = 0;
	*m_right = 0;
	*m_bottom = 0;
	*m_left = 0;
	if (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP)
		*m_top = local_y;
	if (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM)
		*m_bottom = output_h - local_y - surf_h;
	if (has_center) {
		*m_left = local_x - surf_w / 2;
		*m_right = 0;
	} else {
		if (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT)
			*m_left = local_x;
		if (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)
			*m_right = output_w - local_x - surf_w;
	}
}

static void reposition_surface(struct wsk_state *state, int32_t global_x, int32_t global_y, int32_t w, int32_t h) {
	if (!state->outputs)
		return;

	struct wsk_output *target = state->output;
	struct wsk_output *out = state->outputs;
	while (out) {
		if (global_x >= out->x && global_x < out->x + out->logical_w && global_y >= out->y &&
		    global_y < out->y + out->logical_h) {
			target = out;
			break;
		}
		out = out->next;
	}
	if (!target)
		return;

	int32_t local_x = global_x - target->x;
	int32_t local_y = global_y - target->y;

	if (target != state->output) {
		zwlr_layer_surface_v1_destroy(state->layer_surface);
		wl_surface_destroy(state->surface);

		state->surface = wl_compositor_create_surface(state->compositor);
		assert(state->surface);
		wl_surface_add_listener(state->surface, &wl_surface_listener, state);

		struct wl_region *input_region = wl_compositor_create_region(state->compositor);
		wl_surface_set_input_region(state->surface, input_region);
		wl_region_destroy(input_region);

		state->layer_surface =
			zwlr_layer_shell_v1_get_layer_surface(state->layer_shell, state->surface, target->output,
							      ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "showkeys");
		assert(state->layer_surface);
		zwlr_layer_surface_v1_add_listener(state->layer_surface, &layer_surface_listener, state);

		state->width = 0;
		state->height = 0;
		state->resize_pending = false;

		if (state->recording_cairo) {
			cairo_destroy(state->recording_cairo);
			state->recording_cairo = nullptr;
		}
		if (state->recording) {
			cairo_surface_destroy(state->recording);
			state->recording = nullptr;
		}
		if (state->font_options) {
			cairo_font_options_destroy(state->font_options);
			state->font_options = nullptr;
		}
	}

	int32_t m_top, m_right, m_bottom, m_left;
	int32_t eff_w = w > 0 ? w : (int32_t) state->width;
	int32_t eff_h = h > 0 ? h : (int32_t) state->height;
	uint32_t compute_anchor = state->anchor;
	if (eff_w == 0 || eff_h == 0) {
		compute_anchor = 0;
		if (eff_w == 0)
			compute_anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
		if (eff_h == 0)
			compute_anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
	}
	if (state->has_center && eff_w > 0 && eff_h > 0) {
		compute_anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
		compute_anchor &= ~ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	}
	compute_g_margins(compute_anchor, state->has_center, local_x, local_y, target->logical_w, target->logical_h,
			  eff_w, eff_h, &m_top, &m_right, &m_bottom, &m_left);
	zwlr_layer_surface_v1_set_anchor(state->layer_surface, compute_anchor);
	zwlr_layer_surface_v1_set_margin(state->layer_surface, m_top, m_right, m_bottom, m_left);
	zwlr_layer_surface_v1_set_exclusive_zone(state->layer_surface, -1);

	if (w > 0 && h > 0) {
		zwlr_layer_surface_v1_set_size(state->layer_surface, w, h);
	}

	wl_surface_commit(state->surface);
	state->output = target;
}

static void parse_position_arg(const char *arg, int32_t *x, int32_t *y, int32_t *w, int32_t *h) {
	char tmp[256];
	strncpy(tmp, arg, sizeof(tmp) - 1);
	tmp[sizeof(tmp) - 1] = '\0';
	char *sp = strchr(tmp, ' ');
	if (sp)
		*sp = ',';
	char *xsep = strchr(tmp, 'x');
	if (!xsep)
		xsep = strchr(tmp, 'X');
	if (xsep)
		*xsep = ',';
	*x = 0;
	*y = 0;
	*w = 0;
	*h = 0;
	sscanf(tmp, "%d,%d,%d,%d", x, y, w, h);
}

static size_t read_ipc_string(int fd, char *buf, size_t bufsize) {
	size_t pos = 0;
	while (pos < bufsize - 1) {
		ssize_t nr = read(fd, &buf[pos], 1);
		if (nr <= 0)
			break;
		if (buf[pos] == '\0')
			break;
		pos++;
	}
	buf[pos] = '\0';
	return pos;
}

static void apply_opacity_and_reply(struct wsk_state *state, int client_fd, float new_opacity) {
	state->opacity = new_opacity;
	if (state->opacity > 1.0f)
		state->opacity = 1.0f;
	if (state->opacity < 0.01f)
		state->opacity = 0.01f;
	char resp[32];
	int len = snprintf(resp, sizeof(resp), "%g", state->opacity);
	write(client_fd, resp, (size_t) len);
	if (surface_is_configured(state) && state->surface)
		render_frame(state);
}

static void init_state_defaults(struct wsk_state *state) {
	state->margin = DEFAULT_MARGIN;
	strncpy(state->mod_pad, DEFAULT_MOD_PAD, sizeof(state->mod_pad) - 1);
	state->mod_pad[sizeof(state->mod_pad) - 1] = '\0';
	state->background = COLOR_BACKGROUND;
	state->specialfg = COLOR_SPECIAL_FG;
	state->repeatfg = COLOR_REPEAT_FG;
	state->foreground = COLOR_FOREGROUND;
	state->font = DEFAULT_FONT;
	state->timeout = DEFAULT_TIMEOUT;
	state->length_limit = DEFAULT_LENGTH_LIMIT;
	state->fixed_width = 0;
	reset_input_state(state);
	state->last_repeat_time.tv_sec = 0;
	state->last_repeat_time.tv_nsec = 0;
	state->repeat_threshold = REPEAT_THRESHOLD_DEFAULT;
	state->min_height = DISPLAY_MIN_HEIGHT;
	state->mask = (struct mask_state) {0};
	state->inspect = false;
	state->resize_pending = false;
	state->sock_fd = -1;
	state->paused = false;
	state->opacity = DEFAULT_OVERLAY_OPACITY;
	state->text_align = TEXT_ALIGN_LEFT;
	state->sock_path[0] = '\0';
}

static void parse_and_init(struct wsk_state *state, int argc, char *argv[], bool *want_pause, bool *want_resume,
			   bool *want_reload, bool *want_opacity_query, const char **opacity_arg,
			   bool *want_pool, const char **pool_data) {
	bool validate_config = false;
	int c;
	opterr = 0;
	while ((c = getopt(argc, argv, "hib:cf:s:r:F:t:a:m:M:o:l:w:p::D:H:PRKO::dx:g:T:")) != -1) {
		switch (c) {
			case 'l':
				state->length_limit = (int) strtol(optarg, nullptr, 10);
				break;
			case 'w':
				state->fixed_width = (int) strtol(optarg, nullptr, 10);
				break;
			case 'b':
				state->background = parse_color(optarg);
				break;
			case 'f':
				state->foreground = parse_color(optarg);
				break;
			case 's':
				state->specialfg = parse_color(optarg);
				break;
			case 'r':
				state->repeatfg = parse_color(optarg);
				break;
			case 'F':
				state->font = optarg;
				break;
			case 't':
				state->timeout = (int) strtol(optarg, nullptr, 10);
				break;
			case 'a':
				if (!state->has_explicit_anchor)
					state->anchor = 0;
				state->has_explicit_anchor = true;
				if (strcmp(optarg, "top") == 0) {
					state->anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
				} else if (strcmp(optarg, "left") == 0) {
					state->anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
				} else if (strcmp(optarg, "right") == 0) {
					state->anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
				} else if (strcmp(optarg, "bottom") == 0) {
					state->anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
				} else if (strcmp(optarg, "center") == 0) {
					state->has_center = true;
				}
				break;
			case 'm':
				state->margin = (int) strtol(optarg, nullptr, 10);
				state->has_explicit_margin = true;
				break;
			case 'M':
				strncpy(state->mod_pad, optarg, sizeof(state->mod_pad) - 1);
				state->mod_pad[sizeof(state->mod_pad) - 1] = '\0';
				break;
			case 'i':
				state->inspect = true;
				break;
			case 'o':
				state->output_mode = OUTPUT_PINNED;
				state->has_explicit_output = true;
				strncpy(state->target_output_name, optarg, sizeof(state->target_output_name) - 1);
				break;
#ifdef WSK_DEBUG
			case 'D':
				trace_file = fopen(optarg, "w");
				if (!trace_file) {
					fprintf(stderr, "Failed to open trace file: %s\n", optarg);
					exit(1);
				}
				WSK_TRACE("trace started pid=%d", getpid());
				break;
#endif
			case 'H':
				state->min_height = (uint32_t) strtol(optarg, nullptr, 10);
				break;
			case 'P':
				state->paused = true;
				*want_pause = true;
				break;
			case 'R':
				state->paused = false;
				*want_resume = true;
				break;
			case 'O':
				*want_opacity_query = (optarg == nullptr);
				*opacity_arg = optarg;
				break;
			case 'c':
				validate_config = true;
				break;
			case 'K':
				*want_reload = true;
				break;
			case 'p':
				pool_enabled = true;
				apply_pool_colors(optarg);
				*want_pool = true;
				*pool_data = optarg;
				break;
			case 'x':
				state->repeat_threshold = (int) strtol(optarg, nullptr, 10);
				break;
			case 'd':
				state->backspace_delete = true;
				break;
			case 'T':
				if (strcmp(optarg, "left") == 0)
					state->text_align = TEXT_ALIGN_LEFT;
				else if (strcmp(optarg, "center") == 0)
					state->text_align = TEXT_ALIGN_CENTER;
				else if (strcmp(optarg, "right") == 0)
					state->text_align = TEXT_ALIGN_RIGHT;
				else
					fprintf(stderr,
						"wiv: unknown text alignment '%s', use left, center, or right\n",
						optarg);
				break;
			case 'g':
				state->has_target_position = true;
				strncpy(state->target_position_arg, optarg, sizeof(state->target_position_arg) - 1);
				state->target_position_arg[sizeof(state->target_position_arg) - 1] = '\0';
				parse_position_arg(optarg, &state->target_x, &state->target_y, &state->target_w,
						   &state->target_h);
				state->target_slurp_w = state->target_w;
				break;
			case '?':
				fprintf(stderr, "usage: wshowkeys [-b|-f|-s|-r #RRGGBB[AA]] [-F font] "
						"[-t timeout]\n\t[-a top|left|right|bottom|center] [-m margin] [-M modpad] "
						"[-o output] [-l numOfLengthLimit] [-w pixels] [-H padding] [-i] [-P] "
						"[-c] [-R] [-O [opacity]] [-K] [-p[colors]] [-g X,Y[,WxH]] [-T "
						"left|center|right]");
				fprintf(stderr, "\n-c          validate keymap config and exit\n");
				fprintf(stderr, "-K          reload keymap config\n");
				fprintf(stderr, "-p[colors]  toggle (no arg) or set (with colors) the color pool\n");
				fprintf(stderr, "-g X,Y[,WxH]  position overlay at absolute coordinates\n");
				fprintf(stderr, "-M <string>  padding before the key in a modifier combo (off by default)\n");
				exit(1);
		}
	}

	if (!state->has_explicit_anchor) {
		state->anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	} else {
		bool has_vertical =
			(state->anchor & (ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM)) != 0;
		bool has_horizontal = (state->anchor &
				       (ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)) != 0 ||
				      state->has_center;
		if (!has_vertical)
			state->anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
		if (!has_horizontal)
			state->anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	}

	if (state->has_target_position) {
		if (state->has_explicit_margin)
			fprintf(stderr, "wiv: -m ignored when -g is used\n");
		if (state->has_explicit_output)
			fprintf(stderr, "wiv: -o ignored when -g is used\n");
		if (state->fixed_width == 0 && state->target_slurp_w > 1)
			state->fixed_width = state->target_slurp_w;
	}

	if (validate_config) {
		exit(config_validate() == 0 ? 0 : 1);
	}

	if (!pool_enabled) {
		apply_pool_colors(nullptr);
	}

	if (state->fixed_width != 0 && state->target_slurp_w == 0)
		state->fixed_width = compute_fixed_width(state);

	if (*opacity_arg) {
		float val = strtof(*opacity_arg, nullptr);
		if (val < 0.01f)
			val = 0.01f;
		if (val > 1.0f)
			val = 1.0f;
		state->opacity = val;
	}
}

static bool setup_ipc_socket(struct wsk_state *state, bool want_pause, bool want_resume, bool want_reload,
			     bool want_opacity_query, const char *opacity_arg,
			     bool want_pool, const char *pool_arg) {
	char path_buf[256];
	const char *path = get_sock_path(path_buf, sizeof(path_buf));
	strncpy(state->sock_path, path, sizeof(state->sock_path) - 1);
	state->sock_path[sizeof(state->sock_path) - 1] = '\0';

	state->sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (state->sock_fd < 0) {
		fprintf(stderr, "socket: %s\n", strerror(errno));
		return false;
	}

	struct sockaddr_un addr = {0};
	addr.sun_family = AF_UNIX;
	char buf[256];
	snprintf(buf, sizeof(buf), "%s", path);
	memcpy(addr.sun_path, buf, strlen(buf) + 1);

	if (bind(state->sock_fd, (struct sockaddr *) &addr, sizeof(addr)) == 0) {
		if (listen(state->sock_fd, 5) < 0) {
			fprintf(stderr, "listen: %s\n", strerror(errno));
			close(state->sock_fd);
			state->sock_fd = -1;
			return false;
		}
	} else {
		/* Bind failed — check if another instance is running */
		int conn_fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (conn_fd < 0) {
			fprintf(stderr, "socket: %s\n", strerror(errno));
			close(state->sock_fd);
			state->sock_fd = -1;
			return false;
		}
		if (connect(conn_fd, (struct sockaddr *) &addr, sizeof(addr)) == 0) {
			if (state->has_target_position) {
				char cmd = 'M';
				write(conn_fd, &cmd, 1);
				char pos_buf[64];
				snprintf(pos_buf, sizeof(pos_buf), "%d,%d", state->target_x, state->target_y);
				write(conn_fd, pos_buf, strlen(pos_buf));
				char term = '\0';
				write(conn_fd, &term, 1);
				close(conn_fd);
				close(state->sock_fd);
				exit(0);
			}
			if (want_pause) {
				char cmd = 'P';
				write(conn_fd, &cmd, 1);
				close(conn_fd);
				close(state->sock_fd);
				exit(0);
			}
			if (want_resume) {
				char cmd = 'R';
				write(conn_fd, &cmd, 1);
				close(conn_fd);
				close(state->sock_fd);
				exit(0);
			}
			if (opacity_arg || want_opacity_query) {
				char cmd = 'O';
				write(conn_fd, &cmd, 1);
				if (opacity_arg) {
					write(conn_fd, opacity_arg, strlen(opacity_arg));
				}
				char term = '\0';
				write(conn_fd, &term, 1);
				char buf[32] = {0};
				size_t pos = 0;
				while (pos < sizeof(buf) - 1) {
					ssize_t nr = read(conn_fd, &buf[pos], 1);
					if (nr <= 0)
						break;
					if (buf[pos] == '\0')
						break;
					pos++;
				}
				if (pos > 0) {
					printf("%s\n", buf);
				}
				close(conn_fd);
				close(state->sock_fd);
				exit(0);
			}
			if (want_reload) {
				char cmd = 'K';
				write(conn_fd, &cmd, 1);
				close(conn_fd);
				close(state->sock_fd);
				exit(0);
			}
			if (want_pool) {
				char cmd = IPC_CMD_POOL;
				write(conn_fd, &cmd, 1);
				if (pool_arg) {
					write(conn_fd, pool_arg, strlen(pool_arg));
				}
				char term = '\0';
				write(conn_fd, &term, 1);
				close(conn_fd);
				close(state->sock_fd);
				exit(0);
			}
			fprintf(stderr, "wiv: already running, use one of flags -P -R -O -K -g -p, -h for help\n");
			close(conn_fd);
			close(state->sock_fd);
			state->sock_fd = -1;
			return false;
		}
		close(conn_fd);
		/* No other instance — stale socket, unlink and retry */
		unlink(path);
		if (bind(state->sock_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
			fprintf(stderr, "bind: %s\n", strerror(errno));
			close(state->sock_fd);
			state->sock_fd = -1;
			return false;
		}
		if (listen(state->sock_fd, 5) < 0) {
			fprintf(stderr, "listen: %s\n", strerror(errno));
			close(state->sock_fd);
			state->sock_fd = -1;
			return false;
		}
	}
	return true;
}

static bool setup_libinput(struct wsk_state *state) {
	state->udev = udev_new();
	if (!state->udev) {
		fprintf(stderr, "udev_create: %s\n", strerror(errno));
		return false;
	}

	state->libinput = libinput_udev_create_context(&libinput_impl, &state->devmgr, state->udev);
	udev_unref(state->udev);
	if (!state->libinput) {
		fprintf(stderr, "libinput_udev_create_context: %s\n", strerror(errno));
		return false;
	}
	return true;
}

static bool setup_wayland(struct wsk_state *state) {
	state->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!state->xkb_context) {
		fprintf(stderr, "xkb_context_new: %s\n", strerror(errno));
		return false;
	}

	state->display = wl_display_connect(nullptr);
	if (!state->display) {
		fprintf(stderr, "wl_display_connect: %s\n", strerror(errno));
		return false;
	}

	state->registry = wl_display_get_registry(state->display);
	assert(state->registry);
	wl_registry_add_listener(state->registry, &registry_listener, state);
	wl_display_roundtrip(state->display);

	struct {
		const char *name;
		void *ptr;
	} need_globals[] = {
		"wl_compositor", &state->compositor, "wl_shm",		&state->shm,
		"wl_seat",	 &state->seat,	     "wlr_layer_shell", &state->layer_shell,
	};
	for (size_t i = 0; i < sizeof(need_globals) / sizeof(need_globals[0]); ++i) {
		if (!*(void **) need_globals[i].ptr) {
			fprintf(stderr,
				"Error: required Wayland interface '%s' "
				"is not present\n",
				need_globals[i].name);
			return false;
		}
	}

	wl_seat_add_listener(state->seat, &wl_seat_listener, state);
	wl_display_roundtrip(state->display);

	// Resolve startup output for PINNED mode
	struct wl_output *startup_output = nullptr;
	if (state->output_mode == OUTPUT_PINNED) {
		struct wsk_output *wsk_out = state->outputs;
		while (wsk_out) {
			if (strcmp(wsk_out->name, state->target_output_name) == 0) {
				startup_output = wsk_out->output;
				break;
			}
			wsk_out = wsk_out->next;
		}
		if (!startup_output) {
			fprintf(stderr, "output '%s' not found, using default\n", state->target_output_name);
			state->output_mode = OUTPUT_DEFAULT;
		}
	}

	state->surface = wl_compositor_create_surface(state->compositor);
	assert(state->surface);
	wl_surface_add_listener(state->surface, &wl_surface_listener, state);

	struct wl_output *layer_output = nullptr;
	if (state->output_mode == OUTPUT_PINNED)
		layer_output = startup_output;

	state->layer_surface = zwlr_layer_shell_v1_get_layer_surface(state->layer_shell, state->surface, layer_output,
								     ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "showkeys");
	assert(state->layer_surface);

	// 创建空的输入区域
	struct wl_region *input_region = wl_compositor_create_region(state->compositor);
	wl_surface_set_input_region(state->surface, input_region);
	wl_region_destroy(input_region);

	zwlr_layer_surface_v1_add_listener(state->layer_surface, &layer_surface_listener, state);
	zwlr_layer_surface_v1_set_size(state->layer_surface, 1, 1);
	zwlr_layer_surface_v1_set_anchor(state->layer_surface, state->anchor);
	zwlr_layer_surface_v1_set_margin(state->layer_surface, state->margin, state->margin, state->margin,
					 state->margin);
	zwlr_layer_surface_v1_set_exclusive_zone(state->layer_surface, -1);
	wl_surface_commit(state->surface);

	if (state->has_target_position) {
		int32_t gx = 0, gy = 0, gw = 0, gh = 0;
		parse_position_arg(state->target_position_arg, &gx, &gy, &gw, &gh);
		if (gw <= 1)
			gw = 0;
		if (gh <= 1)
			gh = 0;
		if (state->has_center && gw > 0 && gh > 0) {
			gx += gw / 2;
			gy += gh / 2;
		}
		reposition_surface(state, gx, gy, gw, gh);
	}
	return true;
}

static void event_loop(struct wsk_state *state) {
	struct pollfd pollfds[3];
	int npollfds = 2;
	pollfds[0] = (struct pollfd) {
		.fd = libinput_get_fd(state->libinput),
		.events = POLLIN,
	};
	pollfds[1] = (struct pollfd) {
		.fd = wl_display_get_fd(state->display),
		.events = POLLIN,
	};
	if (state->sock_fd >= 0) {
		pollfds[2] = (struct pollfd) {
			.fd = state->sock_fd,
			.events = POLLIN,
		};
		npollfds = 3;
	}

	state->run = true;
	while (state->run) {
		errno = 0;
		do {
			if (wl_display_flush(state->display) == -1 && errno != EAGAIN) {
				fprintf(stderr, "wl_display_flush: %s\n", strerror(errno));
				break;
			}
		} while (errno == EAGAIN);

		int timeout = -1;
		if (!state->paused) {
			if (state->keys) {
				timeout = 200;
			}
			if (state->key_held && (timeout == -1 || REPEAT_RATE < timeout))
				timeout = REPEAT_RATE; /* Wake up often during key hold */
		}

		if (poll(pollfds, npollfds, timeout) < 0) {
			fprintf(stderr, "poll: %s\n", strerror(errno));
			break;
		}
		WSK_TRACE("poll returned revents: libinput=0x%x wayland=0x%x timeout=%d", pollfds[0].revents,
			  pollfds[1].revents, timeout);

		/* Check for fd errors */
		if (pollfds[0].revents & (POLLHUP | POLLERR | POLLNVAL)) {
			fprintf(stderr, "poll fd error: libinput fd revents=%#x\n", pollfds[0].revents);
			break;
		}
		if (pollfds[1].revents & (POLLHUP | POLLERR | POLLNVAL)) {
			fprintf(stderr, "poll fd error: Wayland fd revents=%#x\n", pollfds[1].revents);
			break;
		}
		if (npollfds == 3 && (pollfds[2].revents & (POLLHUP | POLLERR | POLLNVAL))) {
			fprintf(stderr, "poll fd error: IPC socket revents=%#x\n", pollfds[2].revents);
			break;
		}

		if (!state->paused) {
			/* Clear out old keys */
			struct timespec now;
			struct wsk_keypress *key = state->keys;

			clock_gettime(CLOCK_MONOTONIC, &now);
			long elapsed_ns = (now.tv_sec - state->last_key.tv_sec) * 1000000000L +
					  (now.tv_nsec - state->last_key.tv_nsec);
			WSK_TRACE("timeout check: last_was_release=%d elapsed_ms=%ld timeout=%d keys=%p",
				  state->last_was_release, elapsed_ns / 1000000, state->timeout, (void *) key);
			if (state->last_was_release && elapsed_ns > (long) state->timeout * 1000000L) {
				if (state->keys) {
					clear_full_keylink(key, state);
					for (int i = 0; i < state->mask.buffer_len; i++)
						free(state->mask.buffer[i]);
					mask_reset(&state->mask);
				}
			} else {
				int all_key_len = 0;
				const char *prev_display = nullptr;
				bool prev_is_mod = false;
				while (key) {
					const char *display = keypress_display(state, key);
					bool is_mod = find_modifier(key->name) >= 0;
					const char *pad_before = key_pad_before(state, prev_display, prev_is_mod, key->name);
					if (state->fixed_width && state->recording_cairo) {
						int kw = 0, kh = 0;
						int cur_scale = state->output ? state->output->scale : 1;
						get_text_size(state->recording_cairo, state->font, &kw, &kh, nullptr,
							      cur_scale, "%s%s%s", pad_before, display, KEY_PAD_AFTER);
						all_key_len += kw;
					} else {
						all_key_len +=
							strlen(pad_before) + strlen(display) + strlen(KEY_PAD_AFTER);
					}
					prev_display = display;
					prev_is_mod = is_mod;
					key = key->next;
				}
				int limit = state->fixed_width
						    ? state->fixed_width * (state->output ? state->output->scale : 1)
						    : state->length_limit;
				if (all_key_len > limit) {
					key = state->keys;
					struct wsk_keypress *next = key->next;
					free(key);
					state->keys = next;
					set_dirty(state);
				}
			}
		}

		if ((pollfds[0].revents & POLLIN)) {
			if (libinput_dispatch(state->libinput) != 0) {
				fprintf(stderr, "libinput_dispatch: %s\n", strerror(errno));
				break;
			}
			struct libinput_event *event;
			while ((event = libinput_get_event(state->libinput))) {
				if (!state->paused) {
					handle_libinput_event(state, event);
				}
				libinput_event_destroy(event);
				WSK_TRACE("libinput event processed");
			}
		}

		if (!state->paused) {
			/* Synthetic repeat for held key (libinput drops kernel auto-repeat) */
			if (state->key_held && state->prev_combination_key[0] != '\0') {
				struct timespec now;
				clock_gettime(CLOCK_MONOTONIC, &now);
				long elapsed_ms = (now.tv_sec - state->last_key.tv_sec) * 1000L +
						  (now.tv_nsec - state->last_key.tv_nsec) / 1000000L;

				if (elapsed_ms >= REPEAT_DELAY) {
					if (state->repeat_state == 0) {
						state->repeat_state = 1;
						state->last_repeat_time = now;
					}
					if (state->repeat_state == 1) {
						/* First repeat after delay */
						state->repeat_state = 2;
						state->last_repeat_time = now;
						generate_held_key_repeat(state);
					} else if (state->repeat_state == 2) {
						long repeat_elapsed =
							(now.tv_sec - state->last_repeat_time.tv_sec) * 1000L +
							(now.tv_nsec - state->last_repeat_time.tv_nsec) / 1000000L;
						if (repeat_elapsed >= REPEAT_RATE) {
							state->last_repeat_time = now;
							generate_held_key_repeat(state);
						}
					}
				}
			}
		}

		if ((pollfds[1].revents & POLLIN)) {
			int ret_dispatch = wl_display_dispatch(state->display);
			WSK_TRACE("wl_display_dispatch returned %d", ret_dispatch);
			if (ret_dispatch == -1) {
				fprintf(stderr, "wl_display_dispatch: %s\n", strerror(errno));
				break;
			}
		}

		/* Handle IPC socket connections (pause/resume signaling) */
		if (npollfds == 3 && (pollfds[2].revents & POLLIN)) {
			int client_fd = accept(state->sock_fd, nullptr, nullptr);
			if (client_fd >= 0) {
				char cmd = 0;
				ssize_t _nr = read(client_fd, &cmd, 1);
				if (_nr == 1) {
					if (cmd == 'P') {
						clear_full_keylink(state->keys, state);
						reset_input_state(state);
						state->paused = true;
						if (surface_is_configured(state) && state->surface) {
							render_frame(state);
						}
					} else if (cmd == 'R') {
						// Recreate xkb_state to clear any stale modifier state
						// from events dropped during pause
						xkb_state_unref(state->xkb_state);
						state->xkb_state = xkb_state_new(state->xkb_keymap);
						if (!state->xkb_state) {
							fprintf(stderr, "Failed to recreate xkb_state on resume\n");
						}
						state->paused = false;
					} else if (cmd == 'O') {
						char argbuf[16];
						read_ipc_string(client_fd, argbuf, sizeof(argbuf));
						if (argbuf[0] == '\0') {
							char resp[32];
							int len = snprintf(resp, sizeof(resp), "%g", state->opacity);
							write(client_fd, resp, len);
						} else if (argbuf[0] == '+') {
							apply_opacity_and_reply(state, client_fd,
										state->opacity +
											strtof(argbuf + 1, nullptr));
						} else if (argbuf[0] == '-') {
							apply_opacity_and_reply(state, client_fd,
										state->opacity -
											strtof(argbuf + 1, nullptr));
						} else {
							apply_opacity_and_reply(state, client_fd,
										strtof(argbuf, nullptr));
						}
					} else if (cmd == 'K') {
						config_free();
						config_load();
					} else if (cmd == 'M') {
						char argbuf[256];
						read_ipc_string(client_fd, argbuf, sizeof(argbuf));
						int32_t gx = 0, gy = 0;
						if (sscanf(argbuf, "%d,%d", &gx, &gy) >= 2) {
							reposition_surface(state, gx, gy, 0, 0);
							set_dirty(state);
						}
					} else if (cmd == IPC_CMD_POOL) {
						char argbuf[256];
						read_ipc_string(client_fd, argbuf, sizeof(argbuf));
						if (argbuf[0] == '\0') {
							pool_enabled = !pool_enabled;
						} else {
							apply_pool_colors(argbuf);
							pool_enabled = true;
						}
						if (surface_is_configured(state) && state->surface) {
							render_frame(state);
						}
					}
				}
				close(client_fd);
			}
		}

		if (!state->paused) {
			WSK_TRACE("pre-render dirty=%d configured=%d keys=%p", state->dirty,
				  surface_is_configured(state), (void *) state->keys);
			if (state->dirty) {
				state->dirty = false;
				if (surface_is_configured(state) && state->surface) {
					render_frame(state);
				}
			}
		}
	}
}

static void cleanup_state(struct wsk_state *state) {
	if (state->recording_cairo) {
		cairo_destroy(state->recording_cairo);
	}
	if (state->recording) {
		cairo_surface_destroy(state->recording);
	}
	if (state->font_options) {
		cairo_font_options_destroy(state->font_options);
	}
	destroy_buffer(&state->buffer_pool[0]);
	destroy_buffer(&state->buffer_pool[1]);
	WSK_TRACE("shutting down");
#ifdef WSK_DEBUG
	if (trace_file) {
		fclose(trace_file);
	}
#endif
	config_free();
	free(state->repeat_font);

	/* Wayland / XKB / linked-list cleanup */
	struct wsk_output *wsk_out = state->outputs;
	while (wsk_out) {
		struct wsk_output *next = wsk_out->next;
		if (wsk_out->xdg_output)
			zxdg_output_v1_destroy(wsk_out->xdg_output);
		wl_output_destroy(wsk_out->output);
		free(wsk_out);
		wsk_out = next;
	}
	if (state->layer_surface)
		zwlr_layer_surface_v1_destroy(state->layer_surface);
	if (state->layer_shell)
		wl_proxy_destroy((struct wl_proxy *) state->layer_shell);
	if (state->surface)
		wl_surface_destroy(state->surface);
	if (state->keyboard)
		wl_keyboard_destroy(state->keyboard);
	if (state->seat)
		wl_proxy_destroy((struct wl_proxy *) state->seat);
	if (state->compositor)
		wl_compositor_destroy(state->compositor);
	if (state->shm)
		wl_shm_destroy(state->shm);
	if (state->output_mgr)
		wl_proxy_destroy((struct wl_proxy *) state->output_mgr);
	if (state->xkb_state)
		xkb_state_unref(state->xkb_state);
	if (state->xkb_keymap)
		xkb_keymap_unref(state->xkb_keymap);
	if (state->xkb_context)
		xkb_context_unref(state->xkb_context);
	if (state->display)
		wl_display_disconnect(state->display);
}

static void cleanup_socket(struct wsk_state *state) {
	if (state->sock_fd >= 0) {
		close(state->sock_fd);
	}
	unlink(state->sock_path);
	if (state->libinput)
		libinput_unref(state->libinput);
	devmgr_finish(state->devmgr, state->devmgr_pid);
}

int main(int argc, char *argv[]) {
	struct wsk_state state = {0};
	if (devmgr_start(&state.devmgr, &state.devmgr_pid, INPUTDEVPATH) > 0)
		return 1;

	int ret = 0;

	bool want_pause = false;
	bool want_resume = false;
	bool want_reload = false;
	bool want_opacity_query = false;
	const char *opacity_arg = nullptr;
	bool want_pool = false;
	const char *pool_data = nullptr;

	init_state_defaults(&state);
	parse_and_init(&state, argc, argv, &want_pause, &want_resume, &want_reload, &want_opacity_query, &opacity_arg,
		       &want_pool, &pool_data);

	if (!setup_ipc_socket(&state, want_pause, want_resume, want_reload, want_opacity_query, opacity_arg,
			     want_pool, pool_data)) {
		ret = 1;
		goto exit;
	}

	config_load();

	state.repeat_font = scale_font_size(state.font, REPEAT_FONT_SCALE);

	const char *mask_env = getenv("WIV_MASK");
	if (mask_env) {
		char *env_copy = strdup(mask_env);
		if (env_copy) {
			char *tok = strtok(env_copy, ",");
			while (tok && state.mask.num_patterns < MASK_PATTERNS_MAX) {
				strncpy(state.mask.patterns[state.mask.num_patterns], tok, 255);
				state.mask.patterns[state.mask.num_patterns][255] = '\0';
				state.mask.num_patterns++;
				tok = strtok(nullptr, ",");
			}
			free(env_copy);
		}
	}

	if (!setup_libinput(&state)) {
		cleanup_state(&state);
		ret = 1;
		goto exit;
	}
	if (!setup_wayland(&state)) {
		cleanup_state(&state);
		ret = 1;
		goto exit;
	}

	event_loop(&state);

	cleanup_state(&state);
exit:
	cleanup_socket(&state);
	return ret;
}
