#pragma once

#include "Common.h"

#include <string>
#include <vector>

namespace amp
{

// One part of a small resistor/capacitor circuit, between two named points ("nodes").
// "gnd" is ground; the circuit is driven at "in" and read at "out".
struct Part
{
    char kind;  // 'R' (ohms) or 'C' (farads)
    double value;
    std::string a, b;
};

// The circuit as a digital filter at this sample rate: (b, a) with a[0] = 1.
void digitalFilter (const std::vector<Part>& parts, double rate, double sourceOhms, double loadOhms,
                    std::vector<double>& b, std::vector<double>& a);

} // namespace amp
