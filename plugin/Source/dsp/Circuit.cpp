#include "Circuit.h"

#include <complex>
#include <map>
#include <set>

namespace amp
{

using Complex = std::complex<double>;
using Matrix = std::vector<std::vector<double>>;

static constexpr double refHz = 1000.0;  // the circle of test points has this radius

// Solve m x = rhs by Gaussian elimination with partial pivoting; also returns det (m).
static std::vector<Complex> solve (std::vector<std::vector<Complex>> m, std::vector<Complex> rhs, Complex& det)
{
    const size_t n = m.size();
    det = 1.0;

    for (size_t col = 0; col < n; ++col)
    {
        size_t pivot = col;

        for (size_t row = col + 1; row < n; ++row)
            if (std::abs (m[row][col]) > std::abs (m[pivot][col]))
                pivot = row;

        if (pivot != col)
        {
            std::swap (m[pivot], m[col]);
            std::swap (rhs[pivot], rhs[col]);
            det = -det;
        }

        det *= m[col][col];

        if (m[col][col] == 0.0)
            throw PresetError ("circuit: the parts do not make a working circuit (a node is not connected)");

        for (size_t row = col + 1; row < n; ++row)
        {
            const Complex f = m[row][col] / m[col][col];

            for (size_t k = col; k < n; ++k)
                m[row][k] -= f * m[col][k];

            rhs[row] -= f * rhs[col];
        }
    }

    std::vector<Complex> x (n);

    for (size_t i = n; i-- > 0;)
    {
        Complex sum = rhs[i];

        for (size_t k = i + 1; k < n; ++k)
            sum -= m[i][k] * x[k];

        x[i] = sum / m[i][i];
    }

    return x;
}

// Coefficients of H(s) = num(s) / den(s), lowest power first, with den[0] = 1.
static void analogPolynomials (const std::vector<Part>& parts, double sourceOhms, double loadOhms,
                               std::vector<double>& num, std::vector<double>& den)
{
    std::set<std::string> nameSet;

    for (auto& p : parts)
    {
        nameSet.insert (p.a);
        nameSet.insert (p.b);
    }

    nameSet.erase ("gnd");
    std::map<std::string, size_t> index;

    for (auto& name : nameSet)
        index.emplace (name, index.size());

    for (auto* needed : { "in", "out" })
        if (index.count (needed) == 0)
            throw PresetError (juce::String ("circuit has no node called '") + needed + "'");

    const size_t size = index.size();
    Matrix g (size, std::vector<double> (size, 0.0)), c = g;

    auto add = [&] (Matrix& matrix, double value, const std::string& a, const std::string& b)
    {
        for (auto [n, m] : { std::pair { &a, &b }, std::pair { &b, &a } })
        {
            if (*n == "gnd")
                continue;

            matrix[index[*n]][index[*n]] += value;

            if (*m != "gnd")
                matrix[index[*n]][index[*m]] -= value;
        }
    };

    int order = 0;

    for (auto& p : parts)
    {
        if (p.kind == 'R')
            add (g, 1.0 / p.value, p.a, p.b);
        else
        {
            add (c, p.value, p.a, p.b);
            ++order;
        }
    }

    // The source: a voltage behind source_ohms is the same as a current u/source_ohms into "in".
    add (g, 1.0 / sourceOhms, "in", "gnd");
    add (g, 1.0 / loadOhms, "out", "gnd");
    std::vector<Complex> rhs (size, 0.0);
    rhs[index["in"]] = 1.0 / sourceOhms;
    const size_t out = index["out"];

    const int points = 2 * (order + 1);
    const double pi = juce::MathConstants<double>::pi;
    const double w0 = 2 * pi * refHz;
    std::vector<Complex> denValues (points), numValues (points);

    for (int k = 0; k < points; ++k)
    {
        const Complex s = w0 * std::exp (Complex (0, 2 * pi * k / points));
        std::vector<std::vector<Complex>> m (size, std::vector<Complex> (size));

        for (size_t i = 0; i < size; ++i)
            for (size_t j = 0; j < size; ++j)
                m[i][j] = g[i][j] + s * c[i][j];

        Complex d;
        const auto v = solve (m, rhs, d);
        denValues[k] = d;
        numValues[k] = v[out] * d;
    }

    // FFT of the values on the circle = the polynomial coefficients (scaled by w0 ** power).
    num.assign (order + 1, 0.0);
    den.assign (order + 1, 0.0);

    for (int power = 0; power <= order; ++power)
    {
        Complex sumDen = 0.0, sumNum = 0.0;

        for (int k = 0; k < points; ++k)
        {
            const Complex w = std::exp (Complex (0, -2 * pi * power * k / points));
            sumDen += denValues[k] * w;
            sumNum += numValues[k] * w;
        }

        const double scale = std::pow (w0, power);
        den[power] = (sumDen / (double) points / scale).real();
        num[power] = (sumNum / (double) points / scale).real();
    }

    const double d0 = den[0];

    for (int power = 0; power <= order; ++power)
    {
        num[power] /= d0;
        den[power] /= d0;
    }
}

static std::vector<double> convolve (const std::vector<double>& x, const std::vector<double>& y)
{
    std::vector<double> out (x.size() + y.size() - 1, 0.0);

    for (size_t i = 0; i < x.size(); ++i)
        for (size_t j = 0; j < y.size(); ++j)
            out[i + j] += x[i] * y[j];

    return out;
}

void digitalFilter (const std::vector<Part>& parts, double rate, double sourceOhms, double loadOhms,
                    std::vector<double>& b, std::vector<double>& a)
{
    std::vector<double> num, den;
    analogPolynomials (parts, sourceOhms, loadOhms, num, den);

    // Bilinear transform: s = 2*sr*(1 - 1/z)/(1 + 1/z).
    const size_t n = den.size() - 1;
    const double k = 2.0 * rate;
    b.assign (n + 1, 0.0);
    a.assign (n + 1, 0.0);

    for (size_t power = 0; power <= n; ++power)
    {
        std::vector<double> term { std::pow (k, (double) power) };

        for (size_t i = 0; i < power; ++i)
            term = convolve (term, { 1.0, -1.0 });

        for (size_t i = 0; i < n - power; ++i)
            term = convolve (term, { 1.0, 1.0 });

        for (size_t i = 0; i <= n; ++i)
        {
            b[i] += num[power] * term[i];
            a[i] += den[power] * term[i];
        }
    }

    const double a0 = a[0];

    for (size_t i = 0; i <= n; ++i)
    {
        b[i] /= a0;
        a[i] /= a0;
    }
}

} // namespace amp
