// The OSD's screen: bring the display up, and put text on it.
//
// Header-only in interface but not in definition, like `sifclient.hpp`: only
// EE programs use it, and each one that links it gets its own copy of the
// band buffer below.
//
// The geometry is NTSC interlaced, and it is the reference console's own --
// read off a GS register trace rather than chosen (docs/analysis/53). The
// framebuffer is 640x224 in PSMCT32 and SMODE2's FFMD makes both fields read
// the same lines, so the 448-line raster shows each buffer line twice. That
// is what removes the 30 Hz shimmer a 1-pixel horizontal stroke would have on
// a 448-line buffer, and it costs half the memory rather than twice the work.

#pragma once

#include <stdint.h>

namespace ps2::display {

// The framebuffer, in pixels. Its height is half the raster's by design.
inline constexpr uint32_t kWidth = 640;
inline constexpr uint32_t kHeight = 224;

// A glyph is 6x13 and is drawn two pixels wide per source pixel and one tall,
// which on a raster that doubles every line lands as a 12x26 cell -- upright,
// not stretched.
//
// Text is inset from the edges, because a television does not show the whole
// raster: the tube's mask covers the outermost few percent, and a version
// line flush against the left edge is a version line with its first character
// missing. The inset leaves 48 columns by 16 rows, which is more than the
// version line and the field map need.
inline constexpr uint32_t kColumns = 48;
inline constexpr uint32_t kRows = 16;

// Program the CRT controller and the read circuit, and clear the screen.
void begin();

// Put a line of text at a character row, 0 to kRows - 1, or clear that row
// with a null pointer. The string is not copied, so it has to outlive the
// next `present`. Text past the right edge is dropped rather than wrapped: a
// version line that grew too long should look truncated, not reflow into the
// line below.
void setLine(uint32_t row, const char *text);

// Redraw the whole screen from the lines set so far.
//
// Every line is redrawn rather than only the one that changed, because a
// 13-line character row does not align to the transfer's 16-line band: one
// band can hold parts of two rows, and sending a band composed from a single
// row would erase its neighbour.
void present();

// Block until the start of the next vertical blank, so that a program which
// redraws in a loop runs at the raster's pace instead of as fast as the bus
// allows. Returns anyway after a wait far longer than a frame: a machine
// whose GS never reports a blank should not hold the program for ever.
void waitVsync();

}  // namespace ps2::display
