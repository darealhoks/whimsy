#include <math.h>
#include "whimsy.h"

/* oklch hue from the first key byte, at a lightness and chroma held constant so every
 * identity reads the same against the background. srgb out of gamut is clamped */
void whimsy_colour(const uint8_t pk[WHIMSY_PK], uint8_t rgb[3])
{
	double h = pk[0] * (2 * 3.14159265358979 / 256), a = 0.12 * cos(h), b = 0.12 * sin(h);
	double l_ = 0.75 + 0.3963377774 * a + 0.2158037573 * b;
	double m_ = 0.75 - 0.1055613458 * a - 0.0638541728 * b;
	double s_ = 0.75 - 0.0894841775 * a - 1.2914855480 * b;
	double l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;
	double lin[3] = {
		 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
		-1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
		-0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s,
	};
	for (int i = 0; i < 3; i++) {
		double v = lin[i] < 0 ? 0 : lin[i] > 1 ? 1 : lin[i];
		v = v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1 / 2.4) - 0.055;
		rgb[i] = (uint8_t)(v * 255 + 0.5);
	}
}
