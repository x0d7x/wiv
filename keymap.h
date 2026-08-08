#ifndef KEYMAP_H
#define KEYMAP_H

#include <stdint.h>

constexpr const char KEY_PAD_BEFORE[] = "";
constexpr const char KEY_PAD_AFTER[] = "";

constexpr const char REPEAT_MARKER[] = "⋅";
constexpr const char REPEAT_0[] = "0";
constexpr const char REPEAT_1[] = "1";
constexpr const char REPEAT_2[] = "2";
constexpr const char REPEAT_3[] = "3";
constexpr const char REPEAT_4[] = "4";
constexpr const char REPEAT_5[] = "5";
constexpr const char REPEAT_6[] = "6";
constexpr const char REPEAT_7[] = "7";
constexpr const char REPEAT_8[] = "8";
constexpr const char REPEAT_9[] = "9";

constexpr uint32_t COLOR_BACKGROUND = 0x00000000;
constexpr uint32_t COLOR_SPECIAL_FG  = 0xebdbb240;
constexpr uint32_t COLOR_FOREGROUND  = 0xebdbb2f0;
constexpr uint32_t COLOR_REPEAT_FG   = 0xebdbb220;

constexpr const char COLOR_POOL[][10] = {};
constexpr bool POOL_OVERRIDES_FG = false;

constexpr const char DEFAULT_FONT[] = "Sans Bold 40";
constexpr int32_t DISPLAY_MIN_HEIGHT = 100;
constexpr float DEFAULT_OVERLAY_OPACITY = 1.0f;
constexpr int32_t DEFAULT_TIMEOUT = 500;
constexpr int32_t DEFAULT_LENGTH_LIMIT = 20; 
constexpr double REPEAT_FONT_SCALE = 0.5;
constexpr int32_t REPEAT_DELAY = 250;  /* ms */
constexpr int32_t REPEAT_RATE = 40;    /* ms */
constexpr int32_t REPEAT_THRESHOLD_DEFAULT = 3;

#define TEXT_ALIGN_BOTTOM

typedef struct {
	const char *name;
	const char *display;
	const char *fg;
} KeymapEntry;

/* Variadic macro: 2 args → no color override, 3 args → with color */
#define K(...) K_GET_MACRO(__VA_ARGS__, K3, K2)(__VA_ARGS__)
#define K_GET_MACRO(_1, _2, _3, NAME, ...) NAME
#define K2(n, d) {n, d, nullptr}
#define K3(n, d, f) {n, d, f}
#define KC(name, fg) {name, nullptr, fg}

// clang-format off
static const KeymapEntry keymap[] = {
	// K("Return",    "⏎"),
	K("Return",    "⮐"),
	K("space",     "␣", "#ebdbb230"),
	K("Escape",    "󱊷"), // ␛
	K("Control",   ""),
	K("Alt",       ""),
	K("Meta",      "✵"),
	K("Shift",     "⇧"),
	K("Super",     ""),
	K("Tab",       "⮔"),
	K("backslash", "∖"),
	K("BackSpace", "␈"),
	K("Caps_Lock", "⇪"),
	K("Left",      "⇦"),
	K("Up",        "⇧"),
	K("Down",      "⇩"),
	K("Right",     "⇨"),
	K("KP_Insert", "0"),
	K("KP_End",    "1"),
	K("KP_Down",   "2"),
	K("KP_Next",   "3"),
	K("KP_Left",   "4"),
	K("KP_Begin",  "5"),
	K("KP_Right",  "6"),
	K("KP_Home",   "7"),
	K("KP_Up",     "8"),
	K("KP_Prior",  "9"),
	K("KP_Delete", "."),
	K("KP_Enter",  "⏎"),
	// K("0", "⁰"),
	// K("1", "¹"),
	// K("2", "²"),
	// K("3", "³"),
	// K("4", "⁴"),
	// K("5", "⁵"),
	// K("6", "⁶"),
	// K("7", "⁷"),
	// K("8", "⁸"),
	// K("9", "⁹"),
	// K("0", "₀"),
	// K("1", "₁"),
	// K("2", "₂"),
	// K("3", "₃"),
	// K("4", "₄"),
	// K("5", "₅"),
	// K("6", "₆"),
	// K("7", "₇"),
	// K("8", "₈"),
	// K("9", "₉"),

	// COLORS
	// KC("l", "#ff0000")
};
#define KEYMAP_LEN (sizeof(keymap) / sizeof(keymap[0]))

#endif
