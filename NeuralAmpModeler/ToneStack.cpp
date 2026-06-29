#include "ToneStack.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>

namespace
{
constexpr double kMinimumResistance = 1.0;
constexpr double kMinimumPivot = 1.0e-18;

using ToneStackType = dsp::tone_stack::ToneStackType;
using CircuitSpec = dsp::tone_stack::ToneStackCircuitSpec;

struct HiwattToneStackSpec
{
  double inputResistance = 48400.0;
  double treblePotResistance = 220000.0;
  double bassPotResistance = 470000.0;
  double midPotResistance = 100000.0;
  double r1 = 100000.0;
  double r2 = 220000.0;
  double r3 = 22000.0;
  double r4 = 22000.0;
  double loadResistance = 220000.0;
  double c1 = 47e-9;
  double c2 = 1e-9;
  double c3 = 47e-9;
  double c4 = 220e-12;
  double c5 = 1e-9;
};

HiwattToneStackSpec MakeHiwattSpec(const CircuitSpec& editableSpec)
{
  HiwattToneStackSpec spec;
  spec.inputResistance = editableSpec.sourceResistance;
  spec.treblePotResistance = editableSpec.treblePotResistance;
  spec.bassPotResistance = editableSpec.bassPotResistance;
  spec.midPotResistance = editableSpec.midPotResistance;
  spec.loadResistance = editableSpec.loadResistance;
  spec.c3 = editableSpec.bassCapacitance;
  spec.c4 = editableSpec.trebleCapacitance;
  spec.c5 = editableSpec.midCapacitance;
  return spec;
}

CircuitSpec GetDefaultCircuitSpec(ToneStackType type)
{
  // The passive presets are evaluated as the classic three-control FMV/Bassman
  // tone stack mesh. The fields map as:
  // sourceResistance = slope resistor, treble/mid/bass capacitance = C1/C2/C3,
  // treble/bass/mid pot resistance = Rt/Rb/Rm.
  switch (type)
  {
    case ToneStackType::Bench:
    {
      CircuitSpec spec{51000.0, 6.8e-9, 22e-9, 22e-9, 100000.0, 100000.0, 100000.0,
                       1000000.0, 0.20, 0.20, 0.20, 1.0};
      spec.inputResistance = 13000.0;
      return spec;
    }
    case ToneStackType::BigMuff:
    {
      CircuitSpec spec{39000.0, 10e-9, 4e-9, 10e-9, 100000.0, 100000.0, 100000.0,
                       1000000.0, 0.50, 0.50, 0.50, 1.0};
      spec.inputResistance = 1000.0;
      return spec;
    }
    case ToneStackType::Crate:
    {
      CircuitSpec spec{68000.0, 220e-12, 47e-9, 220e-9, 250000.0, 250000.0, 50000.0,
                       1000000.0, 0.50, 0.10, 0.10, 1.0};
      spec.inputResistance = 1000.0;
      return spec;
    }
    case ToneStackType::DmblJazz:
    case ToneStackType::DmblRock:
    {
      CircuitSpec spec{150000.0, 2e-9, 100e-9, 1e-9, 270000.0, 312000.0, 250000.0,
                       1000000.0, 0.10, 0.20, 0.20, 1.0};
      spec.inputResistance = 40000.0;
      return spec;
    }
    case ToneStackType::FndrBassman5F6A:
    {
      CircuitSpec spec{56000.0, 250e-12, 20e-9, 20e-9, 250000.0, 1000000.0, 25000.0,
                       1000000.0, 0.50, 0.10, 0.50, 1.0};
      spec.inputResistance = 1300.0;
      return spec;
    }
    case ToneStackType::FndrBrownface:
    {
      CircuitSpec spec{100000.0, 250e-12, 100e-9, 100e-9, 350000.0, 250000.0, 1000000.0,
                       1000000.0, 0.10, 0.10, 0.50, 1.0};
      spec.inputResistance = 38000.0;
      return spec;
    }
    case ToneStackType::FndrDeluxe5E3:
    {
      CircuitSpec spec{20000.0, 100e-9, 4700e-12, 500e-12, 1000000.0, 1000000.0, 1000000.0,
                       1000000.0, 0.10, 0.10, 0.10, 1.0};
      spec.inputResistance = 20000.0;
      return spec;
    }
    case ToneStackType::FndrESeries:
    {
      CircuitSpec spec{220000.0, 10e-9, 100e-9, 5e-9, 1000000.0, 1000000.0, 1000000.0,
                       1000000.0, 0.10, 0.10, 0.50, 1.0};
      spec.inputResistance = 1300.0;
      return spec;
    }
    case ToneStackType::FndrPrinceton5E2:
    {
      CircuitSpec spec{100000.0, 20e-9, 500e-12, 5e-9, 250000.0, 1000000.0, 1000000.0,
                       1000000.0, 0.10, 0.50, 0.50, 1.0};
      spec.inputResistance = 38000.0;
      return spec;
    }
    case ToneStackType::FndrPrinceton5F2A:
    {
      CircuitSpec spec{1000000.0, 22e-9, 4.7e-9, 500e-12, 1000000.0, 1000000.0, 1000000.0,
                       1000000.0, 0.10, 0.50, 0.50, 1.0};
      spec.inputResistance = 38000.0;
      return spec;
    }
    case ToneStackType::FndrProJr:
    {
      CircuitSpec spec{56000.0, 10e-9, 22e-12, 3.3e-9, 250000.0, 250000.0, 1000000.0,
                       1000000.0, 0.50, 0.50, 0.50, 1.0};
      spec.inputResistance = 38000.0;
      return spec;
    }
    case ToneStackType::FndrTMB:
    {
      CircuitSpec spec{100000.0, 250e-12, 100e-9, 47e-9, 250000.0, 250000.0, 10000.0,
                       1000000.0, 0.10, 0.10, 0.50, 1.0};
      spec.inputResistance = 38000.0;
      return spec;
    }
    case ToneStackType::Marshall:
    {
      CircuitSpec spec{33000.0, 470e-12, 22e-9, 22e-9, 220000.0, 1000000.0, 25000.0,
                       517000.0, 0.50, 0.20, 0.50, 1.0};
      spec.inputResistance = 1300.0;
      return spec;
    }
    case ToneStackType::Neve:
    {
      CircuitSpec spec{6200.0, 22e-9, 15e-9, 15e-9, 10000.0, 50000.0, 1000000.0,
                       1000000.0, 0.50, 0.50, 0.50, 1.0};
      spec.inputResistance = 1.0;
      return spec;
    }
    case ToneStackType::Vox:
    {
      CircuitSpec spec{100000.0, 47e-12, 22e-9, 22e-9, 1000000.0, 1000000.0, 1000000.0,
                       600000.0, 0.10, 0.10, 0.50, 1.0};
      spec.inputResistance = 717.0;
      return spec;
    }
    case ToneStackType::Hiwatt:
    {
      CircuitSpec spec{48400.0, 220e-12, 47e-9, 1e-9, 220000.0, 470000.0, 100000.0, 220000.0,
                       0.50, 0.20, 0.50, 1.0};
      spec.inputResistance = 48400.0;
      return spec;
    }
    case ToneStackType::Default:
    case ToneStackType::Count:
    default: return CircuitSpec{};
  }
}
} // namespace

namespace
{
constexpr int kMaxCircuitOrder = dsp::tone_stack::kToneStackFilterOrder;
using Poly = std::array<double, kMaxCircuitOrder + 1>;

Poly AddPoly(const Poly& a, const Poly& b)
{
  Poly out{};
  for (int i = 0; i <= kMaxCircuitOrder; ++i)
    out[i] = a[i] + b[i];
  return out;
}

Poly SubPoly(const Poly& a, const Poly& b)
{
  Poly out{};
  for (int i = 0; i <= kMaxCircuitOrder; ++i)
    out[i] = a[i] - b[i];
  return out;
}

Poly ScalePoly(const Poly& a, double scale)
{
  Poly out{};
  for (int i = 0; i <= kMaxCircuitOrder; ++i)
    out[i] = a[i] * scale;
  return out;
}

Poly MulPoly(const Poly& a, const Poly& b)
{
  Poly out{};
  for (int i = 0; i <= kMaxCircuitOrder; ++i)
  {
    for (int j = 0; j <= kMaxCircuitOrder - i; ++j)
      out[i + j] += a[i] * b[j];
  }
  return out;
}

Poly Res(double r)
{
  Poly out{};
  out[0] = r;
  return out;
}

Poly CapZ(double c)
{
  Poly out{};
  out[1] = 1.0 / c;
  return out;
}

Poly ReverseXsToS(const Poly& xPoly)
{
  Poly out{};
  for (int i = 0; i <= kMaxCircuitOrder; ++i)
    out[i] = xPoly[kMaxCircuitOrder - i];
  return out;
}

Poly BinomialPow(bool minus, int n)
{
  Poly out{};
  out[0] = 1.0;
  for (int factor = 0; factor < n; ++factor)
  {
    Poly next{};
    for (int i = 0; i <= factor; ++i)
    {
      next[i] += out[i];
      next[i + 1] += minus ? -out[i] : out[i];
    }
    out = next;
  }
  return out;
}

Poly BilinearPolynomial(const Poly& analog, double sampleRate, int order)
{
  const double k = 2.0 * sampleRate;
  Poly out{};
  for (int power = 0; power <= order; ++power)
  {
    const double coefficient = analog[power] * std::pow(k, power);
    const auto minusPart = BinomialPow(true, power);
    const auto plusPart = BinomialPow(false, order - power);
    out = AddPoly(out, ScalePoly(MulPoly(minusPart, plusPart), coefficient));
  }
  return out;
}

Poly Determinant3(const std::array<std::array<Poly, 3>, 3>& m)
{
  const auto a = MulPoly(m[0][0], SubPoly(MulPoly(m[1][1], m[2][2]), MulPoly(m[1][2], m[2][1])));
  const auto b = MulPoly(m[0][1], SubPoly(MulPoly(m[1][0], m[2][2]), MulPoly(m[1][2], m[2][0])));
  const auto c = MulPoly(m[0][2], SubPoly(MulPoly(m[1][0], m[2][1]), MulPoly(m[1][1], m[2][0])));
  return AddPoly(SubPoly(a, b), c);
}

std::array<std::array<Poly, 3>, 3> ReplaceColumn(std::array<std::array<Poly, 3>, 3> matrix, int column,
                                                 const std::array<Poly, 3>& values)
{
  for (int row = 0; row < 3; ++row)
    matrix[row][column] = values[row];
  return matrix;
}

using Complex = std::complex<double>;

template <int N>
std::array<Complex, N> SolveComplexLinearSystem(std::array<std::array<Complex, N>, N> matrix,
                                                std::array<Complex, N> rhs)
{
  constexpr double minimumPivot = 1.0e-24;
  for (int pivot = 0; pivot < N; ++pivot)
  {
    int bestRow = pivot;
    double bestValue = std::abs(matrix[pivot][pivot]);
    for (int row = pivot + 1; row < N; ++row)
    {
      const double candidate = std::abs(matrix[row][pivot]);
      if (candidate > bestValue)
      {
        bestValue = candidate;
        bestRow = row;
      }
    }

    if (bestRow != pivot)
    {
      std::swap(matrix[pivot], matrix[bestRow]);
      std::swap(rhs[pivot], rhs[bestRow]);
    }

    Complex pivotValue = matrix[pivot][pivot];
    if (std::abs(pivotValue) < minimumPivot)
      pivotValue = Complex(minimumPivot, 0.0);

    for (int row = pivot + 1; row < N; ++row)
    {
      const Complex factor = matrix[row][pivot] / pivotValue;
      if (std::abs(factor) == 0.0)
        continue;
      for (int col = pivot; col < N; ++col)
        matrix[row][col] -= factor * matrix[pivot][col];
      rhs[row] -= factor * rhs[pivot];
    }
  }

  std::array<Complex, N> solution{};
  for (int row = N - 1; row >= 0; --row)
  {
    Complex sum = rhs[row];
    for (int col = row + 1; col < N; ++col)
      sum -= matrix[row][col] * solution[col];
    Complex divisor = matrix[row][row];
    if (std::abs(divisor) < minimumPivot)
      divisor = Complex(minimumPivot, 0.0);
    solution[row] = sum / divisor;
  }
  return solution;
}

double LocalPotPosition(double value, double taper)
{
  const double normalized = std::clamp(value / 10.0, 0.001, 0.999);
  const double midpoint = std::clamp(taper, 0.05, 0.95);
  const double exponent = std::log(midpoint) / std::log(0.5);
  return std::clamp(std::pow(normalized, exponent), 0.001, 0.999);
}

Complex EvaluateFmvMna(const CircuitSpec& spec, double bassValue, double midValue, double trebleValue, Complex s)
{
  constexpr int kNodeCount = 6;
  enum Node
  {
    A = 0,
    B,
    T,
    M,
    C,
    O
  };

  std::array<std::array<Complex, kNodeCount>, kNodeCount> y{};
  std::array<Complex, kNodeCount> current{};

  auto stampAdmittance = [&](int a, int b, Complex admittance) {
    if (a >= 0)
      y[a][a] += admittance;
    if (b >= 0)
      y[b][b] += admittance;
    if (a >= 0 && b >= 0)
    {
      y[a][b] -= admittance;
      y[b][a] -= admittance;
    }
  };
  auto stampResistor = [&](int a, int b, double resistance) {
    stampAdmittance(a, b, 1.0 / std::max(1.0, resistance));
  };
  auto stampCapacitor = [&](int a, int b, double capacitance) {
    stampAdmittance(a, b, s * std::max(1.0e-15, capacitance));
  };
  auto stampKnownVoltageThroughResistor = [&](int a, double voltage, double resistance) {
    const double conductance = 1.0 / std::max(1.0, resistance);
    y[a][a] += conductance;
    current[a] += conductance * voltage;
  };

  const double treble = LocalPotPosition(trebleValue, spec.trebleTaper);
  const double mid = LocalPotPosition(midValue, spec.midTaper);
  const double bass = LocalPotPosition(bassValue, spec.bassTaper);

  stampKnownVoltageThroughResistor(A, 1.0, spec.inputResistance);
  stampResistor(A, B, spec.sourceResistance);
  stampCapacitor(A, T, spec.trebleCapacitance);
  stampCapacitor(B, M, spec.midCapacitance);
  stampCapacitor(B, C, spec.bassCapacitance);
  stampResistor(T, O, spec.treblePotResistance * (1.0 - treble) + 1.0);
  stampResistor(O, M, spec.treblePotResistance * treble + 1.0);
  stampResistor(M, C, spec.bassPotResistance * bass + 1.0);
  stampResistor(C, -1, spec.midPotResistance * mid + 1.0);
  stampResistor(O, -1, spec.loadResistance);

  const auto voltages = SolveComplexLinearSystem<kNodeCount>(y, current);
  return voltages[O] * spec.makeupGain;
}

Complex EvaluateClassicTmbMna(const CircuitSpec& spec, double bassValue, double midValue, double trebleValue, Complex s)
{
  // Fender/Marshall-style FMV/TMB stack as shown in the supplied Bassman,
  // Marshall and Fndr TMB schematics:
  // RIN -> R1/slope node, C1 -> treble pot top, C2 -> bass/treble junction,
  // C3 -> mid pot top/wiper path, output at the treble wiper.
  constexpr int kNodeCount = 6;
  enum Node
  {
    IN = 0,
    SLOPE_BOTTOM,
    TREBLE_TOP,
    TONE_JUNCTION,
    MID_BOTTOM,
    OUT
  };

  std::array<std::array<Complex, kNodeCount>, kNodeCount> y{};
  std::array<Complex, kNodeCount> current{};

  auto stampAdmittance = [&](int a, int b, Complex admittance) {
    if (a >= 0)
      y[a][a] += admittance;
    if (b >= 0)
      y[b][b] += admittance;
    if (a >= 0 && b >= 0)
    {
      y[a][b] -= admittance;
      y[b][a] -= admittance;
    }
  };
  auto stampResistor = [&](int a, int b, double resistance) {
    stampAdmittance(a, b, 1.0 / std::max(kMinimumResistance, resistance));
  };
  auto stampCapacitor = [&](int a, int b, double capacitance) {
    stampAdmittance(a, b, s * std::max(1.0e-15, capacitance));
  };
  auto stampKnownVoltageThroughResistor = [&](int a, double voltage, double resistance) {
    const double conductance = 1.0 / std::max(kMinimumResistance, resistance);
    y[a][a] += conductance;
    current[a] += conductance * voltage;
  };

  const double treble = LocalPotPosition(trebleValue, spec.trebleTaper);
  const double mid = LocalPotPosition(midValue, spec.midTaper);
  const double bass = LocalPotPosition(bassValue, spec.bassTaper);

  stampKnownVoltageThroughResistor(IN, 1.0, spec.inputResistance);
  stampResistor(IN, SLOPE_BOTTOM, spec.sourceResistance);
  stampCapacitor(IN, TREBLE_TOP, spec.trebleCapacitance);
  stampCapacitor(SLOPE_BOTTOM, TONE_JUNCTION, spec.midCapacitance);
  stampCapacitor(SLOPE_BOTTOM, MID_BOTTOM, spec.bassCapacitance);

  stampResistor(TREBLE_TOP, OUT, spec.treblePotResistance * (1.0 - treble) + 1.0);
  stampResistor(OUT, TONE_JUNCTION, spec.treblePotResistance * treble + 1.0);

  stampResistor(TONE_JUNCTION, MID_BOTTOM, spec.bassPotResistance * bass + 1.0);
  stampResistor(MID_BOTTOM, -1, spec.midPotResistance * mid + 1.0);
  stampResistor(OUT, -1, spec.loadResistance);

  const auto voltages = SolveComplexLinearSystem<kNodeCount>(y, current);
  return voltages[OUT] * spec.makeupGain;
}

Complex EvaluateHiwattMna(const CircuitSpec& editableSpec, double bassValue, double midValue, double trebleValue, Complex s)
{
  constexpr int kNodeCount = 9;
  enum Node
  {
    IN = 0,
    R1_BOTTOM,
    C2_RIGHT,
    TREBLE_TOP,
    TREBLE_BOTTOM,
    MID_TOP,
    BASS_TOP,
    TREBLE_WIPER,
    OUT
  };

  const HiwattToneStackSpec spec = MakeHiwattSpec(editableSpec);
  std::array<std::array<Complex, kNodeCount>, kNodeCount> y{};
  std::array<Complex, kNodeCount> current{};

  auto stampAdmittance = [&](int a, int b, Complex admittance) {
    if (a >= 0)
      y[a][a] += admittance;
    if (b >= 0)
      y[b][b] += admittance;
    if (a >= 0 && b >= 0)
    {
      y[a][b] -= admittance;
      y[b][a] -= admittance;
    }
  };
  auto stampResistor = [&](int a, int b, double resistance) {
    stampAdmittance(a, b, 1.0 / std::max(kMinimumResistance, resistance));
  };
  auto stampCapacitor = [&](int a, int b, double capacitance) {
    stampAdmittance(a, b, s * std::max(1.0e-15, capacitance));
  };
  auto stampKnownVoltageThroughResistor = [&](int a, double voltage, double resistance) {
    const double conductance = 1.0 / std::max(kMinimumResistance, resistance);
    y[a][a] += conductance;
    current[a] += conductance * voltage;
  };

  const double treble = LocalPotPosition(trebleValue, editableSpec.trebleTaper);
  const double mid = LocalPotPosition(midValue, editableSpec.midTaper);
  const double bass = LocalPotPosition(bassValue, editableSpec.bassTaper);

  stampKnownVoltageThroughResistor(IN, 1.0, spec.inputResistance);
  stampResistor(IN, R1_BOTTOM, spec.r1);
  stampCapacitor(R1_BOTTOM, -1, spec.c1);
  stampCapacitor(IN, C2_RIGHT, spec.c2);
  stampCapacitor(R1_BOTTOM, BASS_TOP, spec.c3);
  stampCapacitor(C2_RIGHT, TREBLE_TOP, spec.c4);
  stampCapacitor(MID_TOP, BASS_TOP, spec.c5);

  stampResistor(C2_RIGHT, TREBLE_BOTTOM, spec.r2);
  stampResistor(TREBLE_BOTTOM, MID_TOP, spec.r3);
  stampResistor(TREBLE_WIPER, OUT, spec.r4);
  stampResistor(OUT, -1, spec.loadResistance);

  stampResistor(TREBLE_TOP, TREBLE_WIPER, spec.treblePotResistance * (1.0 - treble) + 1.0);
  stampResistor(TREBLE_WIPER, TREBLE_BOTTOM, spec.treblePotResistance * treble + 1.0);

  // In the Hiwatt topology the mid and bass wipers are tied to the upper ends of
  // their pots, so each behaves as a variable resistance from that node downward.
  // The orientation below keeps the user control intuitive: higher knob value
  // means less shunt and therefore more of that band.
  stampResistor(MID_TOP, BASS_TOP, spec.midPotResistance * mid + 1.0);
  stampResistor(BASS_TOP, -1, spec.bassPotResistance * bass + 1.0);

  const auto voltages = SolveComplexLinearSystem<kNodeCount>(y, current);
  return voltages[OUT] * editableSpec.makeupGain;
}

Complex EvaluateVoxMna(const CircuitSpec& spec, double bassValue, double trebleValue, Complex s)
{
  constexpr int kNodeCount = 6;
  enum Node
  {
    IN = 0,
    R1_BOTTOM,
    TREBLE_TOP,
    TREBLE_BOTTOM,
    BASS_WIPER,
    OUT
  };

  std::array<std::array<Complex, kNodeCount>, kNodeCount> y{};
  std::array<Complex, kNodeCount> current{};

  auto stampAdmittance = [&](int a, int b, Complex admittance) {
    if (a >= 0)
      y[a][a] += admittance;
    if (b >= 0)
      y[b][b] += admittance;
    if (a >= 0 && b >= 0)
    {
      y[a][b] -= admittance;
      y[b][a] -= admittance;
    }
  };
  auto stampResistor = [&](int a, int b, double resistance) {
    stampAdmittance(a, b, 1.0 / std::max(kMinimumResistance, resistance));
  };
  auto stampCapacitor = [&](int a, int b, double capacitance) {
    stampAdmittance(a, b, s * std::max(1.0e-15, capacitance));
  };
  auto stampKnownVoltageThroughResistor = [&](int a, double voltage, double resistance) {
    const double conductance = 1.0 / std::max(kMinimumResistance, resistance);
    y[a][a] += conductance;
    current[a] += conductance * voltage;
  };

  const double treble = LocalPotPosition(trebleValue, spec.trebleTaper);
  const double bass = LocalPotPosition(bassValue, spec.bassTaper);

  stampKnownVoltageThroughResistor(IN, 1.0, spec.inputResistance);
  stampResistor(IN, R1_BOTTOM, spec.sourceResistance);
  stampResistor(BASS_WIPER, -1, 10000.0); // R2 from the provided Vox schematic
  stampCapacitor(IN, TREBLE_TOP, spec.trebleCapacitance);
  stampCapacitor(R1_BOTTOM, TREBLE_BOTTOM, spec.midCapacitance);
  stampCapacitor(R1_BOTTOM, BASS_WIPER, spec.bassCapacitance);

  stampResistor(TREBLE_TOP, OUT, spec.treblePotResistance * (1.0 - treble) + 1.0);
  stampResistor(OUT, TREBLE_BOTTOM, spec.treblePotResistance * treble + 1.0);

  // Vox bass control: the wiper is connected to the C3/R2 node, not to the output.
  stampResistor(TREBLE_BOTTOM, BASS_WIPER, spec.bassPotResistance * bass + 1.0);
  stampResistor(BASS_WIPER, -1, spec.bassPotResistance * (1.0 - bass) + 1.0);
  stampResistor(OUT, -1, spec.loadResistance);

  const auto voltages = SolveComplexLinearSystem<kNodeCount>(y, current);
  return voltages[OUT] * spec.makeupGain;
}

Complex EvaluateCrateMna(const CircuitSpec& spec, double bassValue, double midValue, double trebleValue, Complex s)
{
  constexpr int kNodeCount = 8;
  enum Node
  {
    IN = 0,
    R1_BOTTOM,
    TREBLE_SERIES,
    TREBLE_TOP,
    STACK,
    BASS_BOTTOM,
    MID_NODE,
    OUT
  };

  std::array<std::array<Complex, kNodeCount>, kNodeCount> y{};
  std::array<Complex, kNodeCount> current{};

  auto stampAdmittance = [&](int a, int b, Complex admittance) {
    if (a >= 0)
      y[a][a] += admittance;
    if (b >= 0)
      y[b][b] += admittance;
    if (a >= 0 && b >= 0)
    {
      y[a][b] -= admittance;
      y[b][a] -= admittance;
    }
  };
  auto stampResistor = [&](int a, int b, double resistance) {
    stampAdmittance(a, b, 1.0 / std::max(kMinimumResistance, resistance));
  };
  auto stampCapacitor = [&](int a, int b, double capacitance) {
    stampAdmittance(a, b, s * std::max(1.0e-15, capacitance));
  };
  auto stampKnownVoltageThroughResistor = [&](int a, double voltage, double resistance) {
    const double conductance = 1.0 / std::max(kMinimumResistance, resistance);
    y[a][a] += conductance;
    current[a] += conductance * voltage;
  };

  const double treble = LocalPotPosition(trebleValue, spec.trebleTaper);
  const double mid = LocalPotPosition(midValue, spec.midTaper);
  const double bass = LocalPotPosition(bassValue, spec.bassTaper);

  stampKnownVoltageThroughResistor(IN, 1.0, spec.inputResistance);
  stampResistor(IN, R1_BOTTOM, spec.sourceResistance);
  stampCapacitor(IN, TREBLE_SERIES, spec.trebleCapacitance);
  stampResistor(TREBLE_SERIES, TREBLE_TOP, 22000.0); // R3
  stampCapacitor(R1_BOTTOM, STACK, spec.midCapacitance);
  stampCapacitor(R1_BOTTOM, MID_NODE, spec.bassCapacitance);
  stampResistor(MID_NODE, -1, 47000.0); // R2
  stampCapacitor(MID_NODE, -1, 4.7e-9); // C4

  stampResistor(MID_NODE, -1, spec.midPotResistance * mid + 1.0);
  stampResistor(TREBLE_TOP, OUT, spec.treblePotResistance * (1.0 - treble) + 1.0);
  stampResistor(OUT, STACK, spec.treblePotResistance * treble + 1.0);

  stampResistor(STACK, BASS_BOTTOM, spec.bassPotResistance * bass + 1.0);
  stampResistor(BASS_BOTTOM, -1, 10000.0); // R4
  stampResistor(OUT, -1, spec.loadResistance);

  const auto voltages = SolveComplexLinearSystem<kNodeCount>(y, current);
  return voltages[OUT] * spec.makeupGain;
}

Complex EvaluateBenchMna(const CircuitSpec& spec, double bassValue, double midValue, double trebleValue, Complex s)
{
  constexpr int kNodeCount = 8;
  enum Node
  {
    IN = 0,
    BASS_WIPER,
    MID_WIPER,
    TREBLE_WIPER,
    MID_CAP,
    TREBLE_CAP,
    OUT,
    LOW_RAIL
  };

  std::array<std::array<Complex, kNodeCount>, kNodeCount> y{};
  std::array<Complex, kNodeCount> current{};

  auto stampAdmittance = [&](int a, int b, Complex admittance) {
    if (a >= 0)
      y[a][a] += admittance;
    if (b >= 0)
      y[b][b] += admittance;
    if (a >= 0 && b >= 0)
    {
      y[a][b] -= admittance;
      y[b][a] -= admittance;
    }
  };
  auto stampResistor = [&](int a, int b, double resistance) {
    stampAdmittance(a, b, 1.0 / std::max(kMinimumResistance, resistance));
  };
  auto stampCapacitor = [&](int a, int b, double capacitance) {
    stampAdmittance(a, b, s * std::max(1.0e-15, capacitance));
  };
  auto stampInductor = [&](int a, int b, double inductance) {
    stampAdmittance(a, b, 1.0 / (s * std::max(1.0e-9, inductance)));
  };
  auto stampKnownVoltageThroughResistor = [&](int a, double voltage, double resistance) {
    const double conductance = 1.0 / std::max(kMinimumResistance, resistance);
    y[a][a] += conductance;
    current[a] += conductance * voltage;
  };

  const double treble = LocalPotPosition(trebleValue, spec.trebleTaper);
  const double mid = LocalPotPosition(midValue, spec.midTaper);
  const double bass = LocalPotPosition(bassValue, spec.bassTaper);
  constexpr double r5 = 5100.0;
  constexpr double l1 = 6.0;
  constexpr double l2 = 20.0;

  stampKnownVoltageThroughResistor(IN, 1.0, spec.inputResistance);
  stampResistor(LOW_RAIL, -1, r5);
  stampResistor(IN, OUT, spec.sourceResistance);

  auto stampPotToGround = [&](double resistance, double position, int wiper) {
    stampResistor(IN, wiper, resistance * (1.0 - position) + 1.0);
    stampResistor(wiper, -1, resistance * position + 1.0);
  };
  stampPotToGround(spec.bassPotResistance, bass, BASS_WIPER);
  stampPotToGround(spec.midPotResistance, mid, MID_WIPER);
  stampPotToGround(spec.treblePotResistance, treble, TREBLE_WIPER);

  stampInductor(BASS_WIPER, OUT, l2);
  stampCapacitor(MID_WIPER, MID_CAP, spec.bassCapacitance);
  stampInductor(MID_CAP, OUT, l1);
  stampCapacitor(TREBLE_WIPER, TREBLE_CAP, spec.trebleCapacitance);
  stampResistor(TREBLE_CAP, OUT, kMinimumResistance);
  stampResistor(OUT, LOW_RAIL, kMinimumResistance);
  stampResistor(OUT, -1, spec.loadResistance);

  const auto voltages = SolveComplexLinearSystem<kNodeCount>(y, current);
  return voltages[OUT] * spec.makeupGain;
}

Complex EvaluateBigMuffMna(const CircuitSpec& spec, double midValue, Complex s)
{
  constexpr int kNodeCount = 5;
  enum Node
  {
    IN = 0,
    LOW_NODE,
    HIGH_NODE,
    OUT,
    TOP
  };

  std::array<std::array<Complex, kNodeCount>, kNodeCount> y{};
  std::array<Complex, kNodeCount> current{};

  auto stampAdmittance = [&](int a, int b, Complex admittance) {
    if (a >= 0)
      y[a][a] += admittance;
    if (b >= 0)
      y[b][b] += admittance;
    if (a >= 0 && b >= 0)
    {
      y[a][b] -= admittance;
      y[b][a] -= admittance;
    }
  };
  auto stampResistor = [&](int a, int b, double resistance) {
    stampAdmittance(a, b, 1.0 / std::max(kMinimumResistance, resistance));
  };
  auto stampCapacitor = [&](int a, int b, double capacitance) {
    stampAdmittance(a, b, s * std::max(1.0e-15, capacitance));
  };
  auto stampKnownVoltageThroughResistor = [&](int a, double voltage, double resistance) {
    const double conductance = 1.0 / std::max(kMinimumResistance, resistance);
    y[a][a] += conductance;
    current[a] += conductance * voltage;
  };

  const double tone = LocalPotPosition(midValue, spec.midTaper);
  constexpr double r2 = 22000.0;

  stampKnownVoltageThroughResistor(TOP, 1.0, spec.inputResistance);
  stampCapacitor(TOP, LOW_NODE, spec.bassCapacitance);
  stampResistor(LOW_NODE, -1, r2);
  stampResistor(TOP, HIGH_NODE, spec.sourceResistance);
  stampCapacitor(HIGH_NODE, -1, spec.trebleCapacitance);
  stampResistor(LOW_NODE, OUT, spec.midPotResistance * (1.0 - tone) + 1.0);
  stampResistor(OUT, HIGH_NODE, spec.midPotResistance * tone + 1.0);
  stampResistor(OUT, -1, spec.loadResistance);

  const auto voltages = SolveComplexLinearSystem<kNodeCount>(y, current);
  return voltages[OUT] * spec.makeupGain;
}

Complex EvaluateDumbleMna(ToneStackType type, const CircuitSpec& spec, double bassValue, double midValue,
                          double trebleValue, Complex s)
{
  constexpr int kNodeCount = 9;
  enum Node
  {
    IN = 0,
    R1_BOTTOM,
    BASS_TOP,
    BASS_WIPER,
    MID_TOP,
    TREBLE_FEED,
    TREBLE_TOP,
    VOLUME_WIPER,
    OUT
  };

  std::array<std::array<Complex, kNodeCount>, kNodeCount> y{};
  std::array<Complex, kNodeCount> current{};

  auto stampAdmittance = [&](int a, int b, Complex admittance) {
    if (a >= 0)
      y[a][a] += admittance;
    if (b >= 0)
      y[b][b] += admittance;
    if (a >= 0 && b >= 0)
    {
      y[a][b] -= admittance;
      y[b][a] -= admittance;
    }
  };
  auto stampResistor = [&](int a, int b, double resistance) {
    stampAdmittance(a, b, 1.0 / std::max(kMinimumResistance, resistance));
  };
  auto stampCapacitor = [&](int a, int b, double capacitance) {
    stampAdmittance(a, b, s * std::max(1.0e-15, capacitance));
  };
  auto stampKnownVoltageThroughResistor = [&](int a, double voltage, double resistance) {
    const double conductance = 1.0 / std::max(kMinimumResistance, resistance);
    y[a][a] += conductance;
    current[a] += conductance * voltage;
  };

  const double treble = LocalPotPosition(trebleValue, spec.trebleTaper);
  const double mid = LocalPotPosition(midValue, spec.midTaper);
  const double bass = LocalPotPosition(bassValue, spec.bassTaper);

  const bool jazz = type == ToneStackType::DmblJazz;
  const double r2 = 10000.0;
  const double r3 = 4700000.0;
  const double r4 = jazz ? 100000.0 : 1.0;
  const double volumePot = 1000000.0;
  const double c4 = jazz ? 4.7e-9 : 1.0e-9;
  const double c5 = 390e-12;
  const double c6 = 220e-12;
  const double c7 = jazz ? 1.0e-9 : 1.0e-15;

  stampKnownVoltageThroughResistor(IN, 1.0, spec.inputResistance);
  stampResistor(IN, R1_BOTTOM, spec.sourceResistance);
  stampCapacitor(IN, TREBLE_FEED, spec.trebleCapacitance);
  stampResistor(TREBLE_FEED, -1, r3);

  stampCapacitor(R1_BOTTOM, BASS_TOP, spec.bassCapacitance);
  stampCapacitor(R1_BOTTOM, MID_TOP, 10e-9);
  stampCapacitor(BASS_TOP, BASS_WIPER, spec.midCapacitance);
  stampResistor(BASS_WIPER, -1, spec.bassPotResistance * (0.25 + 0.75 * bass) + 1.0);
  stampResistor(BASS_TOP, -1, r2);
  stampCapacitor(BASS_WIPER, -1, c4);

  stampResistor(MID_TOP, -1, spec.midPotResistance * mid + 1.0);
  if (jazz)
  {
    stampCapacitor(BASS_WIPER, MID_TOP, c7);
    stampResistor(BASS_WIPER, TREBLE_TOP, r4);
  }
  else
  {
    stampResistor(BASS_WIPER, TREBLE_TOP, kMinimumResistance);
  }

  stampCapacitor(TREBLE_FEED, TREBLE_TOP, c5);
  stampResistor(TREBLE_TOP, VOLUME_WIPER, spec.treblePotResistance * (1.0 - treble) + 1.0);
  stampResistor(VOLUME_WIPER, BASS_WIPER, spec.treblePotResistance * treble + 1.0);

  // The Dumble schematics include a post-stack volume. Keep it fixed at 10 as requested.
  stampResistor(VOLUME_WIPER, OUT, 1.0);
  stampResistor(OUT, -1, volumePot + 1.0);
  stampCapacitor(OUT, -1, c6);
  stampResistor(OUT, -1, spec.loadResistance);

  const auto voltages = SolveComplexLinearSystem<kNodeCount>(y, current);
  return voltages[OUT] * spec.makeupGain;
}

Complex EvaluateFenderBrownfaceMna(const CircuitSpec& spec, double bassValue, double trebleValue, Complex s)
{
  constexpr int kNodeCount = 7;
  enum Node
  {
    IN = 0,
    R1_BOTTOM,
    TREBLE_TOP,
    TONE_NODE,
    BASS_BOTTOM,
    TREBLE_BOTTOM,
    OUT
  };

  std::array<std::array<Complex, kNodeCount>, kNodeCount> y{};
  std::array<Complex, kNodeCount> current{};

  auto stampAdmittance = [&](int a, int b, Complex admittance) {
    if (a >= 0)
      y[a][a] += admittance;
    if (b >= 0)
      y[b][b] += admittance;
    if (a >= 0 && b >= 0)
    {
      y[a][b] -= admittance;
      y[b][a] -= admittance;
    }
  };
  auto stampResistor = [&](int a, int b, double resistance) {
    stampAdmittance(a, b, 1.0 / std::max(kMinimumResistance, resistance));
  };
  auto stampCapacitor = [&](int a, int b, double capacitance) {
    stampAdmittance(a, b, s * std::max(1.0e-15, capacitance));
  };
  auto stampKnownVoltageThroughResistor = [&](int a, double voltage, double resistance) {
    const double conductance = 1.0 / std::max(kMinimumResistance, resistance);
    y[a][a] += conductance;
    current[a] += conductance * voltage;
  };

  const double treble = LocalPotPosition(trebleValue, spec.trebleTaper);
  const double bass = LocalPotPosition(bassValue, spec.bassTaper);
  const double r2 = 6800.0;
  const double rTap = 70000.0;
  const double c4 = spec.midCapacitance;

  stampKnownVoltageThroughResistor(IN, 1.0, spec.inputResistance);
  stampResistor(IN, R1_BOTTOM, spec.sourceResistance);
  stampCapacitor(IN, TREBLE_TOP, spec.trebleCapacitance);
  stampCapacitor(R1_BOTTOM, TONE_NODE, spec.bassCapacitance);
  stampCapacitor(R1_BOTTOM, BASS_BOTTOM, 100e-9);
  stampResistor(TONE_NODE, BASS_BOTTOM, spec.bassPotResistance * bass + 1.0);
  stampResistor(BASS_BOTTOM, -1, r2);
  stampResistor(TREBLE_TOP, OUT, spec.treblePotResistance * (1.0 - treble) + 1.0);
  stampResistor(OUT, TREBLE_BOTTOM, spec.treblePotResistance * treble + 1.0);
  stampResistor(TREBLE_BOTTOM, -1, rTap);
  stampCapacitor(TREBLE_BOTTOM, -1, c4);
  stampResistor(OUT, -1, spec.loadResistance);

  const auto voltages = SolveComplexLinearSystem<kNodeCount>(y, current);
  return voltages[OUT] * spec.makeupGain;
}

Complex EvaluateFenderNoMidMna(ToneStackType type, const CircuitSpec& spec, double bassValue, double trebleValue,
                               Complex s)
{
  if (type == ToneStackType::FndrBrownface)
    return EvaluateFenderBrownfaceMna(spec, bassValue, trebleValue, s);

  constexpr int kNodeCount = 7;
  enum Node
  {
    IN = 0,
    COUPLED,
    TONE_TOP,
    TONE_WIPER,
    TREBLE_BRANCH,
    BASS_BRANCH,
    OUT
  };

  std::array<std::array<Complex, kNodeCount>, kNodeCount> y{};
  std::array<Complex, kNodeCount> current{};

  auto stampAdmittance = [&](int a, int b, Complex admittance) {
    if (a >= 0)
      y[a][a] += admittance;
    if (b >= 0)
      y[b][b] += admittance;
    if (a >= 0 && b >= 0)
    {
      y[a][b] -= admittance;
      y[b][a] -= admittance;
    }
  };
  auto stampResistor = [&](int a, int b, double resistance) {
    stampAdmittance(a, b, 1.0 / std::max(kMinimumResistance, resistance));
  };
  auto stampCapacitor = [&](int a, int b, double capacitance) {
    stampAdmittance(a, b, s * std::max(1.0e-15, capacitance));
  };
  auto stampKnownVoltageThroughResistor = [&](int a, double voltage, double resistance) {
    const double conductance = 1.0 / std::max(kMinimumResistance, resistance);
    y[a][a] += conductance;
    current[a] += conductance * voltage;
  };

  const double treble = LocalPotPosition(trebleValue, spec.trebleTaper);
  const double bass = LocalPotPosition(bassValue, spec.bassTaper);

  const bool hasBassControl = type != ToneStackType::FndrPrinceton5E2 &&
                              type != ToneStackType::FndrPrinceton5F2A &&
                              type != ToneStackType::FndrProJr;
  const double effectiveBass = hasBassControl ? bass : 0.999;
  const double rv = type == ToneStackType::FndrProJr ? 250000.0 : 1000000.0;
  const double seriesR = type == ToneStackType::FndrProJr ? 100.0 : spec.inputResistance;
  const double trebleShuntCap = type == ToneStackType::FndrDeluxe5E3 ? 500e-12 : spec.midCapacitance;

  stampKnownVoltageThroughResistor(IN, 1.0, spec.inputResistance);
  stampCapacitor(IN, COUPLED, spec.trebleCapacitance);
  stampResistor(COUPLED, TONE_TOP, seriesR);
  stampCapacitor(TONE_TOP, TREBLE_BRANCH, spec.midCapacitance);
  stampCapacitor(TONE_TOP, BASS_BRANCH, spec.bassCapacitance);
  stampResistor(TREBLE_BRANCH, -1, spec.treblePotResistance * treble + 1.0);
  stampCapacitor(TREBLE_BRANCH, -1, trebleShuntCap);
  stampResistor(BASS_BRANCH, -1, rv * effectiveBass + 1.0);
  stampResistor(TONE_TOP, TONE_WIPER, spec.treblePotResistance * (1.0 - treble) + 1.0);
  stampResistor(TONE_WIPER, OUT, kMinimumResistance);
  stampResistor(OUT, -1, spec.loadResistance);

  const auto voltages = SolveComplexLinearSystem<kNodeCount>(y, current);
  return voltages[OUT] * spec.makeupGain;
}

Complex EvaluateNeveMna(const CircuitSpec& spec, double bassValue, double trebleValue, Complex s)
{
  constexpr int kNodeCount = 10;
  enum Node
  {
    IN = 0,
    BASS_TOP,
    BASS_BOTTOM,
    BASS_WIPER,
    TREBLE_R4_BOTTOM,
    TREBLE_TOP,
    TREBLE_BOTTOM,
    TREBLE_WIPER,
    OUT,
    C6_NODE
  };

  std::array<std::array<Complex, kNodeCount>, kNodeCount> y{};
  std::array<Complex, kNodeCount> current{};

  auto stampAdmittance = [&](int a, int b, Complex admittance) {
    if (a >= 0)
      y[a][a] += admittance;
    if (b >= 0)
      y[b][b] += admittance;
    if (a >= 0 && b >= 0)
    {
      y[a][b] -= admittance;
      y[b][a] -= admittance;
    }
  };
  auto stampResistor = [&](int a, int b, double resistance) {
    stampAdmittance(a, b, 1.0 / std::max(kMinimumResistance, resistance));
  };
  auto stampCapacitor = [&](int a, int b, double capacitance) {
    stampAdmittance(a, b, s * std::max(1.0e-15, capacitance));
  };
  auto stampKnownVoltageThroughResistor = [&](int a, double voltage, double resistance) {
    const double conductance = 1.0 / std::max(kMinimumResistance, resistance);
    y[a][a] += conductance;
    current[a] += conductance * voltage;
  };

  const double treble = LocalPotPosition(trebleValue, spec.trebleTaper);
  const double bass = LocalPotPosition(bassValue, spec.bassTaper);
  constexpr double r2 = 6200.0;
  constexpr double r3 = 12000.0;
  constexpr double r4 = 620.0;
  constexpr double r5 = 620.0;
  constexpr double r6 = 12000.0;
  constexpr double c1 = 15e-9;
  constexpr double c2 = 15e-9;
  constexpr double c3 = 15e-9;
  constexpr double c4 = 22e-9;
  constexpr double c5 = 22e-9;
  constexpr double c6 = 10e-9;
  constexpr double cf = 470e-12;

  stampKnownVoltageThroughResistor(IN, 1.0, spec.inputResistance);
  stampResistor(IN, BASS_TOP, spec.sourceResistance);
  stampResistor(BASS_BOTTOM, -1, r2);
  stampResistor(BASS_TOP, BASS_WIPER, spec.bassPotResistance * (1.0 - bass) + 1.0);
  stampResistor(BASS_WIPER, BASS_BOTTOM, spec.bassPotResistance * bass + 1.0);
  stampCapacitor(BASS_TOP, BASS_WIPER, c1);
  stampCapacitor(BASS_WIPER, BASS_BOTTOM, c2);
  stampCapacitor(BASS_TOP, C6_NODE, c6);
  stampResistor(C6_NODE, -1, r6);
  stampResistor(BASS_WIPER, OUT, r3);

  stampResistor(IN, TREBLE_R4_BOTTOM, r4);
  stampCapacitor(TREBLE_R4_BOTTOM, TREBLE_TOP, c4);
  stampResistor(TREBLE_TOP, TREBLE_WIPER, spec.treblePotResistance * (1.0 - treble) + 1.0);
  stampResistor(TREBLE_WIPER, TREBLE_BOTTOM, spec.treblePotResistance * treble + 1.0);
  stampCapacitor(TREBLE_BOTTOM, -1, c5);
  stampResistor(TREBLE_BOTTOM, -1, r5);
  stampCapacitor(TREBLE_WIPER, OUT, c3);

  stampCapacitor(OUT, -1, cf);
  stampResistor(OUT, -1, spec.loadResistance);

  const auto voltages = SolveComplexLinearSystem<kNodeCount>(y, current);
  return voltages[OUT] * spec.makeupGain;
}

Complex EvaluateToneStackMna(ToneStackType type, const CircuitSpec& spec, double bassValue, double midValue,
                             double trebleValue, Complex s)
{
  switch (type)
  {
    case ToneStackType::Bench: return EvaluateBenchMna(spec, bassValue, midValue, trebleValue, s);
    case ToneStackType::BigMuff: return EvaluateBigMuffMna(spec, midValue, s);
    case ToneStackType::Crate: return EvaluateCrateMna(spec, bassValue, midValue, trebleValue, s);
    case ToneStackType::DmblJazz:
    case ToneStackType::DmblRock: return EvaluateDumbleMna(type, spec, bassValue, midValue, trebleValue, s);
    case ToneStackType::FndrBassman5F6A:
    case ToneStackType::FndrTMB:
    case ToneStackType::Marshall: return EvaluateClassicTmbMna(spec, bassValue, midValue, trebleValue, s);
    case ToneStackType::FndrBrownface:
    case ToneStackType::FndrESeries:
    case ToneStackType::FndrPrinceton5E2:
    case ToneStackType::FndrPrinceton5F2A:
    case ToneStackType::FndrProJr: return EvaluateFenderNoMidMna(type, spec, bassValue, trebleValue, s);
    case ToneStackType::FndrDeluxe5E3: return EvaluateFenderNoMidMna(type, spec, trebleValue, bassValue, s);
    case ToneStackType::Hiwatt: return EvaluateHiwattMna(spec, bassValue, midValue, trebleValue, s);
    case ToneStackType::Vox: return EvaluateVoxMna(spec, bassValue, trebleValue, s);
    case ToneStackType::Neve: return EvaluateNeveMna(spec, bassValue, trebleValue, s);
    case ToneStackType::Default:
    case ToneStackType::Count:
    default: return Complex(1.0, 0.0);
  }
}

template <int Order>
bool FitAnalogTransferFromMnaOrder(ToneStackType type, const CircuitSpec& spec, double bassValue, double midValue,
                                   double trebleValue, Poly& numerator, Poly& denominator)
{
  constexpr int kUnknowns = 2 * Order + 1;
  constexpr int kFitSize = 17;
  constexpr double kScaleFrequency = 2.0 * 3.1415926535897932384626433832795 * 1000.0;
  constexpr std::array<double, kFitSize> kAllFitFrequencies{{20.0, 31.5, 50.0, 80.0, 125.0, 200.0,
                                                             315.0, 500.0, 800.0, 1250.0, 2000.0, 3150.0,
                                                             5000.0, 8000.0, 12000.0, 16000.0, 20000.0}};

  std::array<std::array<Complex, kUnknowns>, kUnknowns> normalMatrix{};
  std::array<Complex, kUnknowns> normalRhs{};

  for (int row = 0; row < kFitSize; ++row)
  {
    const Complex s(0.0, 2.0 * 3.1415926535897932384626433832795 * kAllFitFrequencies[row]);
    const Complex p = s / kScaleFrequency;
    const Complex h = EvaluateToneStackMna(type, spec, bassValue, midValue, trebleValue, s);
    std::array<Complex, kUnknowns> rowValues{};
    Complex pPower(1.0, 0.0);
    for (int coefficient = 0; coefficient <= Order; ++coefficient)
    {
      rowValues[coefficient] = pPower;
      pPower *= p;
    }
    pPower = p;
    for (int coefficient = 1; coefficient <= Order; ++coefficient)
    {
      rowValues[Order + coefficient] = -h * pPower;
      pPower *= p;
    }

    // Mild mid-band emphasis keeps the practical guitar range tight without
    // forcing exact interpolation at the extremes.
    const double weight = kAllFitFrequencies[row] >= 80.0 && kAllFitFrequencies[row] <= 8000.0 ? 1.0 : 0.5;
    for (int r = 0; r < kUnknowns; ++r)
    {
      const Complex weightedConjugate = std::conj(rowValues[r]) * weight;
      normalRhs[r] += weightedConjugate * h;
      for (int c = 0; c < kUnknowns; ++c)
        normalMatrix[r][c] += weightedConjugate * rowValues[c];
    }
  }

  const auto fit = SolveComplexLinearSystem<kUnknowns>(normalMatrix, normalRhs);
  numerator = {};
  denominator = {};
  for (int i = 0; i <= Order; ++i)
    numerator[i] = fit[i].real() / std::pow(kScaleFrequency, i);
  denominator[0] = 1.0;
  for (int i = 1; i <= Order; ++i)
    denominator[i] = fit[Order + i].real() / std::pow(kScaleFrequency, i);

  bool valid = true;
  for (int i = 0; i <= Order; ++i)
    valid = valid && std::isfinite(numerator[i]) && std::isfinite(denominator[i]);
  return valid;
}

bool IsFinitePoly(const Poly& poly)
{
  for (const double value : poly)
  {
    if (!std::isfinite(value))
      return false;
  }
  return true;
}

double MeasureImpulsePeak(const Poly& b, const Poly& a)
{
  std::array<double, kMaxCircuitOrder> x{};
  std::array<double, kMaxCircuitOrder> y{};
  double peak = 0.0;
  for (int sample = 0; sample < 512; ++sample)
  {
    const double input = sample == 0 ? 1.0 : 0.0;
    double output = b[0] * input;
    for (int i = 1; i <= kMaxCircuitOrder; ++i)
      output += b[i] * x[i - 1] - a[i] * y[i - 1];

    for (int i = kMaxCircuitOrder - 1; i > 0; --i)
    {
      x[i] = x[i - 1];
      y[i] = y[i - 1];
    }
    x[0] = input;
    y[0] = output;

    if (!std::isfinite(output))
      return std::numeric_limits<double>::infinity();
    peak = std::max(peak, std::abs(output));
  }
  return peak;
}

template <int Order>
bool TryBuildDigitalToneStackFilter(ToneStackType type, const CircuitSpec& spec, double bassValue, double midValue,
                                    double trebleValue, double sampleRate, double normalizationGain, Poly& b, Poly& a)
{
  Poly numeratorS{};
  Poly denominatorS{};
  if (!FitAnalogTransferFromMnaOrder<Order>(type, spec, bassValue, midValue, trebleValue, numeratorS, denominatorS))
    return false;

  numeratorS = ScalePoly(numeratorS, normalizationGain);

  Poly candidateB = BilinearPolynomial(numeratorS, sampleRate, Order);
  Poly candidateA = BilinearPolynomial(denominatorS, sampleRate, Order);
  const double a0 = std::abs(candidateA[0]) < kMinimumPivot
                      ? (candidateA[0] < 0.0 ? -kMinimumPivot : kMinimumPivot)
                      : candidateA[0];
  for (int i = 0; i <= kMaxCircuitOrder; ++i)
  {
    candidateB[i] /= a0;
    candidateA[i] /= a0;
  }

  if (!IsFinitePoly(candidateB) || !IsFinitePoly(candidateA))
    return false;

  constexpr double kMaxAcceptedImpulsePeak = 20.0;
  const double impulsePeak = MeasureImpulsePeak(candidateB, candidateA);
  if (!std::isfinite(impulsePeak) || impulsePeak > kMaxAcceptedImpulsePeak)
    return false;

  b = candidateB;
  a = candidateA;
  return true;
}

bool BuildDigitalToneStackFilter(ToneStackType type, const CircuitSpec& spec, double bassValue, double midValue,
                                 double trebleValue, double sampleRate, double normalizationGain, Poly& b, Poly& a)
{
  return TryBuildDigitalToneStackFilter<1>(type, spec, bassValue, midValue, trebleValue, sampleRate, normalizationGain,
                                           b, a) ||
         TryBuildDigitalToneStackFilter<2>(type, spec, bassValue, midValue, trebleValue, sampleRate, normalizationGain,
                                           b, a) ||
         TryBuildDigitalToneStackFilter<3>(type, spec, bassValue, midValue, trebleValue, sampleRate, normalizationGain,
                                           b, a) ||
         TryBuildDigitalToneStackFilter<4>(type, spec, bassValue, midValue, trebleValue, sampleRate, normalizationGain,
                                           b, a) ||
         TryBuildDigitalToneStackFilter<5>(type, spec, bassValue, midValue, trebleValue, sampleRate, normalizationGain,
                                           b, a);
}

bool FitAnalogTransferFromMna(ToneStackType type, const CircuitSpec& spec, double bassValue, double midValue,
                              double trebleValue, Poly& numerator, Poly& denominator, int& order)
{
  if (type == ToneStackType::Hiwatt)
  {
    order = 5;
    return FitAnalogTransferFromMnaOrder<5>(type, spec, bassValue, midValue, trebleValue, numerator, denominator);
  }
  order = 3;
  return FitAnalogTransferFromMnaOrder<3>(type, spec, bassValue, midValue, trebleValue, numerator, denominator);
}
} // namespace

const char* dsp::tone_stack::GetToneStackTypeName(ToneStackType type)
{
  switch (type)
  {
    case ToneStackType::Default: return "Default";
    case ToneStackType::Bench: return "Bench";
    case ToneStackType::BigMuff: return "Big Muff";
    case ToneStackType::Crate: return "Crate";
    case ToneStackType::DmblJazz: return "Dmbl Jazz";
    case ToneStackType::DmblRock: return "Dmbl Rock";
    case ToneStackType::FndrBassman5F6A: return "Fndr Bassman 5F6-A";
    case ToneStackType::FndrBrownface: return "Fndr Brownface";
    case ToneStackType::FndrDeluxe5E3: return "Fndr Deluxe 5E3";
    case ToneStackType::FndrESeries: return "Fndr E-series";
    case ToneStackType::FndrPrinceton5E2: return "Fndr Princeton 5E2";
    case ToneStackType::FndrPrinceton5F2A: return "Fndr Princeton 5F2A";
    case ToneStackType::FndrProJr: return "Fndr Pro Jr";
    case ToneStackType::FndrTMB: return "Fndr TMB";
    case ToneStackType::Hiwatt: return "Hiwatt";
    case ToneStackType::Marshall: return "Marshall";
    case ToneStackType::Neve: return "Neve";
    case ToneStackType::Vox: return "Vox";
    case ToneStackType::Count:
    default: return "Default";
  }
}

dsp::tone_stack::ToneStackType dsp::tone_stack::ToneStackTypeFromInt(int value)
{
  value = std::clamp(value, 0, kNumToneStackTypes - 1);
  return static_cast<ToneStackType>(value);
}

bool dsp::tone_stack::ToneStackTypeHasBassControl(ToneStackType type)
{
  switch (type)
  {
    case ToneStackType::BigMuff:
    case ToneStackType::FndrPrinceton5E2:
    case ToneStackType::FndrPrinceton5F2A:
    case ToneStackType::FndrProJr: return false;
    case ToneStackType::Bench:
    case ToneStackType::Default:
    case ToneStackType::Crate:
    case ToneStackType::DmblJazz:
    case ToneStackType::DmblRock:
    case ToneStackType::FndrBassman5F6A:
    case ToneStackType::FndrBrownface:
    case ToneStackType::FndrDeluxe5E3:
    case ToneStackType::FndrESeries:
    case ToneStackType::FndrTMB:
    case ToneStackType::Hiwatt:
    case ToneStackType::Marshall:
    case ToneStackType::Neve:
    case ToneStackType::Vox:
    case ToneStackType::Count:
    default: return true;
  }
}

bool dsp::tone_stack::ToneStackTypeHasMiddleControl(ToneStackType type)
{
  switch (type)
  {
    case ToneStackType::Crate:
    case ToneStackType::Bench:
    case ToneStackType::BigMuff:
    case ToneStackType::DmblJazz:
    case ToneStackType::DmblRock:
    case ToneStackType::FndrBassman5F6A:
    case ToneStackType::FndrTMB:
    case ToneStackType::Hiwatt:
    case ToneStackType::Marshall: return true;
    case ToneStackType::Default: return true;
    case ToneStackType::FndrBrownface:
    case ToneStackType::FndrDeluxe5E3:
    case ToneStackType::FndrESeries:
    case ToneStackType::FndrPrinceton5E2:
    case ToneStackType::FndrPrinceton5F2A:
    case ToneStackType::FndrProJr:
    case ToneStackType::Vox:
    case ToneStackType::Neve:
    case ToneStackType::Count:
    default: return false;
  }
}

bool dsp::tone_stack::ToneStackTypeHasTrebleControl(ToneStackType type)
{
  switch (type)
  {
    case ToneStackType::BigMuff: return false;
    case ToneStackType::Default:
    case ToneStackType::Bench:
    case ToneStackType::Crate:
    case ToneStackType::DmblJazz:
    case ToneStackType::DmblRock:
    case ToneStackType::FndrBassman5F6A:
    case ToneStackType::FndrBrownface:
    case ToneStackType::FndrDeluxe5E3:
    case ToneStackType::FndrESeries:
    case ToneStackType::FndrPrinceton5E2:
    case ToneStackType::FndrPrinceton5F2A:
    case ToneStackType::FndrProJr:
    case ToneStackType::FndrTMB:
    case ToneStackType::Hiwatt:
    case ToneStackType::Marshall:
    case ToneStackType::Neve:
    case ToneStackType::Vox:
    case ToneStackType::Count:
    default: return true;
  }
}

const char* dsp::tone_stack::GetToneStackComponentName(ToneStackComponent component)
{
  switch (component)
  {
    case ToneStackComponent::SlopeResistor: return "Slope R";
    case ToneStackComponent::TrebleCap: return "Treble C";
    case ToneStackComponent::MidCap: return "Mid C";
    case ToneStackComponent::BassCap: return "Bass C";
    case ToneStackComponent::TreblePot: return "Treble Pot";
    case ToneStackComponent::MidPot: return "Mid Pot";
    case ToneStackComponent::BassPot: return "Bass Pot";
    case ToneStackComponent::LoadResistor: return "Load R";
    case ToneStackComponent::TrebleTaper: return "Treble Taper";
    case ToneStackComponent::MidTaper: return "Mid Taper";
    case ToneStackComponent::BassTaper: return "Bass Taper";
    case ToneStackComponent::MakeupGain: return "Gain";
    case ToneStackComponent::Count:
    default: return "";
  }
}

const char* dsp::tone_stack::GetToneStackComponentUnit(ToneStackComponent component)
{
  switch (component)
  {
    case ToneStackComponent::TrebleCap:
    case ToneStackComponent::MidCap:
    case ToneStackComponent::BassCap: return "nF";
    case ToneStackComponent::TrebleTaper:
    case ToneStackComponent::MidTaper:
    case ToneStackComponent::BassTaper: return "";
    case ToneStackComponent::MakeupGain: return "x";
    default: return "k";
  }
}

dsp::tone_stack::ToneStackComponent dsp::tone_stack::ToneStackComponentFromInt(int value)
{
  value = std::clamp(value, 0, kNumToneStackComponents - 1);
  return static_cast<ToneStackComponent>(value);
}

bool dsp::tone_stack::ToneStackTypeHasComponent(ToneStackType type, ToneStackComponent component)
{
  if (type == ToneStackType::Default)
    return false;

  const bool hasBass = ToneStackTypeHasBassControl(type);
  const bool hasMiddle = ToneStackTypeHasMiddleControl(type);
  const bool hasTreble = ToneStackTypeHasTrebleControl(type);
  switch (component)
  {
    case ToneStackComponent::BassPot:
    case ToneStackComponent::BassTaper: return hasBass;
    case ToneStackComponent::MidPot:
    case ToneStackComponent::MidTaper: return hasMiddle;
    case ToneStackComponent::TreblePot:
    case ToneStackComponent::TrebleTaper: return hasTreble;
    case ToneStackComponent::SlopeResistor:
    case ToneStackComponent::TrebleCap:
    case ToneStackComponent::MidCap:
    case ToneStackComponent::BassCap:
    case ToneStackComponent::LoadResistor:
    case ToneStackComponent::MakeupGain: return true;
    case ToneStackComponent::Count:
    default: return false;
  }
}

DSP_SAMPLE** dsp::tone_stack::BasicNamToneStack::Process(DSP_SAMPLE** inputs, const int numChannels,
                                                         const int numFrames)
{
  if (mToneStackType != ToneStackType::Default)
    return _ProcessCircuit(inputs, numChannels, numFrames);

  DSP_SAMPLE** bassPointers = mToneBass.Process(inputs, numChannels, numFrames);
  DSP_SAMPLE** midPointers = mToneMid.Process(bassPointers, numChannels, numFrames);
  DSP_SAMPLE** treblePointers = mToneTreble.Process(midPointers, numChannels, numFrames);
  return treblePointers;
}

void dsp::tone_stack::BasicNamToneStack::_ResetCircuitSpecsToDefaults()
{
  for (int i = 0; i < kNumToneStackTypes; ++i)
    mCircuitSpecs[i] = _DefaultCircuitSpec(ToneStackTypeFromInt(i));
  mCircuitSpecsInitialized = true;
  mCircuitNormalizationDirty = true;
}

dsp::tone_stack::ToneStackCircuitSpec dsp::tone_stack::BasicNamToneStack::_DefaultCircuitSpec(ToneStackType type) const
{
  return GetDefaultCircuitSpec(type);
}

void dsp::tone_stack::BasicNamToneStack::Reset(const double sampleRate, const int maxBlockSize)
{
  dsp::tone_stack::AbstractToneStack::Reset(sampleRate, maxBlockSize);
  if (!mCircuitSpecsInitialized)
    _ResetCircuitSpecsToDefaults();
  mOutputData.assign(2 * std::max(1, maxBlockSize), 0.0f);
  mOutputPointers.resize(2);
  for (int ch = 0; ch < 2; ++ch)
    mOutputPointers[ch] = mOutputData.data() + ch * std::max(1, maxBlockSize);
  mCircuitStates.assign(2, ChannelCircuitState{});

  // Refresh the params!
  _RefreshAllParams();
}

double dsp::tone_stack::BasicNamToneStack::GetComponentValue(int type, int component) const
{
  const auto stackType = ToneStackTypeFromInt(type);
  const auto stackComponent = ToneStackComponentFromInt(component);
  if (!ToneStackTypeHasComponent(stackType, stackComponent))
    return 0.0;
  const auto& spec = mCircuitSpecs[static_cast<int>(stackType)];
  switch (stackComponent)
  {
    case ToneStackComponent::SlopeResistor: return spec.sourceResistance / 1000.0;
    case ToneStackComponent::TrebleCap: return spec.trebleCapacitance * 1.0e9;
    case ToneStackComponent::MidCap: return spec.midCapacitance * 1.0e9;
    case ToneStackComponent::BassCap: return spec.bassCapacitance * 1.0e9;
    case ToneStackComponent::TreblePot: return spec.treblePotResistance / 1000.0;
    case ToneStackComponent::MidPot: return spec.midPotResistance / 1000.0;
    case ToneStackComponent::BassPot: return spec.bassPotResistance / 1000.0;
    case ToneStackComponent::LoadResistor: return spec.loadResistance / 1000.0;
    case ToneStackComponent::TrebleTaper: return spec.trebleTaper;
    case ToneStackComponent::MidTaper: return spec.midTaper;
    case ToneStackComponent::BassTaper: return spec.bassTaper;
    case ToneStackComponent::MakeupGain: return spec.makeupGain;
    case ToneStackComponent::Count:
    default: return 0.0;
  }
}

void dsp::tone_stack::BasicNamToneStack::SetComponentValue(int type, int component, double value)
{
  if (!mCircuitSpecsInitialized)
    _ResetCircuitSpecsToDefaults();

  const auto stackType = ToneStackTypeFromInt(type);
  const auto stackComponent = ToneStackComponentFromInt(component);
  if (!ToneStackTypeHasComponent(stackType, stackComponent))
    return;
  auto& spec = mCircuitSpecs[static_cast<int>(stackType)];
  value = std::max(0.000001, value);
  switch (stackComponent)
  {
    case ToneStackComponent::SlopeResistor: spec.sourceResistance = value * 1000.0; break;
    case ToneStackComponent::TrebleCap: spec.trebleCapacitance = value * 1.0e-9; break;
    case ToneStackComponent::MidCap: spec.midCapacitance = value * 1.0e-9; break;
    case ToneStackComponent::BassCap: spec.bassCapacitance = value * 1.0e-9; break;
    case ToneStackComponent::TreblePot: spec.treblePotResistance = value * 1000.0; break;
    case ToneStackComponent::MidPot: spec.midPotResistance = value * 1000.0; break;
    case ToneStackComponent::BassPot: spec.bassPotResistance = value * 1000.0; break;
    case ToneStackComponent::LoadResistor: spec.loadResistance = value * 1000.0; break;
    case ToneStackComponent::TrebleTaper: spec.trebleTaper = std::clamp(value, 0.05, 0.95); break;
    case ToneStackComponent::MidTaper: spec.midTaper = std::clamp(value, 0.05, 0.95); break;
    case ToneStackComponent::BassTaper: spec.bassTaper = std::clamp(value, 0.05, 0.95); break;
    case ToneStackComponent::MakeupGain: spec.makeupGain = std::clamp(value, 0.01, 100.0); break;
    case ToneStackComponent::Count:
    default: break;
  }

  if (stackType == mToneStackType)
  {
    mCircuitNormalizationDirty = true;
    _RefreshAllParams();
  }
}

void dsp::tone_stack::BasicNamToneStack::ResetComponentValues(int type)
{
  if (!mCircuitSpecsInitialized)
    _ResetCircuitSpecsToDefaults();

  const auto stackType = ToneStackTypeFromInt(type);
  mCircuitSpecs[static_cast<int>(stackType)] = _DefaultCircuitSpec(stackType);
  if (stackType == mToneStackType)
  {
    mCircuitNormalizationDirty = true;
    _RefreshAllParams();
  }
}

void dsp::tone_stack::BasicNamToneStack::SetParam(const std::string name, const double val)
{
  if (name == "bass")
  {
    mBassVal = val;
  }
  else if (name == "middle")
  {
    mMiddleVal = val;
  }
  else if (name == "treble")
  {
    mTrebleVal = val;
  }
  else if (name == "type")
  {
    _SetToneStackType(static_cast<int>(std::lround(val)));
    return;
  }

  _RefreshAllParams();
}

void dsp::tone_stack::BasicNamToneStack::_SetToneStackType(const int type)
{
  const auto newType = ToneStackTypeFromInt(type);
  if (newType == mToneStackType)
    return;

  mToneStackType = newType;
  mCircuitNormalizationDirty = true;
  for (auto& channelState : mCircuitStates)
    channelState = ChannelCircuitState{};
  _RefreshAllParams();
}

void dsp::tone_stack::BasicNamToneStack::_RefreshAllParams()
{
  if (mToneStackType == ToneStackType::Default)
    _RefreshClassicEQParams();
  else
    _RefreshCircuit();
}

void dsp::tone_stack::BasicNamToneStack::_RefreshClassicEQParams()
{
  const double sampleRate = GetSampleRate();
  if (sampleRate <= 0.0)
    return;

  const double bassGainDB = 4.0 * (mBassVal - 5.0); // +/- 20
  recursive_linear_filter::BiquadParams bassParams(sampleRate, 150.0, 0.707, bassGainDB);
  mToneBass.SetParams(bassParams);

  const double midGainDB = 3.0 * (mMiddleVal - 5.0); // +/- 15
  const double midQuality = midGainDB < 0.0 ? 1.5 : 0.7;
  recursive_linear_filter::BiquadParams midParams(sampleRate, 425.0, midQuality, midGainDB);
  mToneMid.SetParams(midParams);

  const double trebleGainDB = 2.0 * (mTrebleVal - 5.0); // +/- 10
  recursive_linear_filter::BiquadParams trebleParams(sampleRate, 1800.0, 0.707, trebleGainDB);
  mToneTreble.SetParams(trebleParams);
}

void dsp::tone_stack::BasicNamToneStack::_RefreshCircuit()
{
  _RefreshFmvFilter();
}

void dsp::tone_stack::BasicNamToneStack::_RefreshCircuitNormalization()
{
  if (!mCircuitNormalizationDirty)
    return;

  mCircuitNormalizationGain = 1.0;
  const auto& spec = _GetCircuitSpec();
  auto unityReferenceSpec = spec;
  unityReferenceSpec.makeupGain = 1.0;
  const Complex unityReference = EvaluateToneStackMna(
    mToneStackType, unityReferenceSpec, 5.0, 5.0, 5.0,
    Complex(0.0, 2.0 * 3.1415926535897932384626433832795 * 1000.0));
  const double unityReferenceMagnitude = std::abs(unityReference);
  if (std::isfinite(unityReferenceMagnitude) && unityReferenceMagnitude > 1.0e-9)
    mCircuitNormalizationGain = 1.0 / unityReferenceMagnitude;

  mCircuitNormalizationDirty = false;
}

void dsp::tone_stack::BasicNamToneStack::_RefreshFmvFilter()
{
  if (!mCircuitSpecsInitialized)
    _ResetCircuitSpecsToDefaults();

  const auto oldB = mCircuitB;
  const auto oldA = mCircuitA;

  const auto& spec = _GetCircuitSpec();
  const double sampleRate = GetSampleRate();
  if (sampleRate <= 0.0)
    return;

  _RefreshCircuitNormalization();
  std::array<double, kToneStackFilterOrder + 1> newB{};
  std::array<double, kToneStackFilterOrder + 1> newA{};
  if (!BuildDigitalToneStackFilter(mToneStackType, spec, mBassVal, mMiddleVal, mTrebleVal, sampleRate,
                                   mCircuitNormalizationGain, newB, newA))
  {
    newB[0] = 1.0;
    newA[0] = 1.0;
  }

  bool changed = false;
  for (int i = 0; i <= kToneStackFilterOrder; ++i)
  {
    changed = changed || std::abs(newB[i] - oldB[i]) > 1.0e-12 || std::abs(newA[i] - oldA[i]) > 1.0e-12;
  }

  mPreviousCircuitB = oldB;
  mPreviousCircuitA = oldA;
  mCircuitB = newB;
  mCircuitA = newA;

  if (changed)
  {
    constexpr int kToneStackTransitionSamples = 128;
    for (auto& channelState : mCircuitStates)
    {
      channelState.previousX = channelState.x;
      channelState.previousY = channelState.y;
      channelState.transitionSamplesRemaining = kToneStackTransitionSamples;
    }
    return;
  }
}

const dsp::tone_stack::ToneStackCircuitSpec& dsp::tone_stack::BasicNamToneStack::_GetCircuitSpec() const
{
  return mCircuitSpecs[static_cast<int>(mToneStackType)];
}

double dsp::tone_stack::BasicNamToneStack::_PotPosition(double value, double taper)
{
  const double normalized = std::clamp(value / 10.0, 0.001, 0.999);
  const double midpoint = std::clamp(taper, 0.05, 0.95);
  const double exponent = std::log(midpoint) / std::log(0.5);
  return std::clamp(std::pow(normalized, exponent), 0.001, 0.999);
}

double dsp::tone_stack::BasicNamToneStack::_SafeResistance(double value)
{
  return std::max(kMinimumResistance, value);
}

void dsp::tone_stack::BasicNamToneStack::_StampConductance(std::array<std::array<double, 5>, 5>& matrix, int a, int b,
                                                           double conductance) const
{
  if (a >= 0)
    matrix[a][a] += conductance;
  if (b >= 0)
    matrix[b][b] += conductance;
  if (a >= 0 && b >= 0)
  {
    matrix[a][b] -= conductance;
    matrix[b][a] -= conductance;
  }
}

void dsp::tone_stack::BasicNamToneStack::_StampCurrent(std::array<double, 5>& rhs, int a, int b, double current) const
{
  if (a >= 0)
    rhs[a] -= current;
  if (b >= 0)
    rhs[b] += current;
}

std::array<double, 5> dsp::tone_stack::BasicNamToneStack::_SolveCircuit(std::array<std::array<double, 5>, 5> matrix,
                                                                        std::array<double, 5> rhs) const
{
  for (int pivot = 0; pivot < 5; ++pivot)
  {
    int bestRow = pivot;
    double bestValue = std::abs(matrix[pivot][pivot]);
    for (int row = pivot + 1; row < 5; ++row)
    {
      const double candidate = std::abs(matrix[row][pivot]);
      if (candidate > bestValue)
      {
        bestValue = candidate;
        bestRow = row;
      }
    }

    if (bestRow != pivot)
    {
      std::swap(matrix[pivot], matrix[bestRow]);
      std::swap(rhs[pivot], rhs[bestRow]);
    }

    const double pivotValue = std::abs(matrix[pivot][pivot]) < kMinimumPivot
                                ? (matrix[pivot][pivot] < 0.0 ? -kMinimumPivot : kMinimumPivot)
                                : matrix[pivot][pivot];
    for (int row = pivot + 1; row < 5; ++row)
    {
      const double factor = matrix[row][pivot] / pivotValue;
      if (factor == 0.0)
        continue;
      for (int col = pivot; col < 5; ++col)
        matrix[row][col] -= factor * matrix[pivot][col];
      rhs[row] -= factor * rhs[pivot];
    }
  }

  std::array<double, 5> solution{};
  for (int row = 4; row >= 0; --row)
  {
    double sum = rhs[row];
    for (int col = row + 1; col < 5; ++col)
      sum -= matrix[row][col] * solution[col];
    const double divisor = std::abs(matrix[row][row]) < kMinimumPivot
                             ? (matrix[row][row] < 0.0 ? -kMinimumPivot : kMinimumPivot)
                             : matrix[row][row];
    solution[row] = sum / divisor;
  }

  return solution;
}

void dsp::tone_stack::BasicNamToneStack::_FactorCircuitMatrix()
{
  mFactoredCircuitMatrix = mBaseCircuitMatrix;
  for (int pivot = 0; pivot < 5; ++pivot)
  {
    int bestRow = pivot;
    double bestValue = std::abs(mFactoredCircuitMatrix[pivot][pivot]);
    for (int row = pivot + 1; row < 5; ++row)
    {
      const double candidate = std::abs(mFactoredCircuitMatrix[row][pivot]);
      if (candidate > bestValue)
      {
        bestValue = candidate;
        bestRow = row;
      }
    }

    mCircuitPivotRows[pivot] = bestRow;
    if (bestRow != pivot)
      std::swap(mFactoredCircuitMatrix[pivot], mFactoredCircuitMatrix[bestRow]);

    double pivotValue = mFactoredCircuitMatrix[pivot][pivot];
    if (std::abs(pivotValue) < kMinimumPivot)
    {
      pivotValue = pivotValue < 0.0 ? -kMinimumPivot : kMinimumPivot;
      mFactoredCircuitMatrix[pivot][pivot] = pivotValue;
    }

    for (int row = pivot + 1; row < 5; ++row)
    {
      const double factor = mFactoredCircuitMatrix[row][pivot] / pivotValue;
      mFactoredCircuitMatrix[row][pivot] = factor;
      for (int col = pivot + 1; col < 5; ++col)
        mFactoredCircuitMatrix[row][col] -= factor * mFactoredCircuitMatrix[pivot][col];
    }
  }
}

std::array<double, 5> dsp::tone_stack::BasicNamToneStack::_SolveFactoredCircuit(std::array<double, 5> rhs) const
{
  for (int pivot = 0; pivot < 5; ++pivot)
  {
    const int bestRow = mCircuitPivotRows[pivot];
    if (bestRow != pivot)
      std::swap(rhs[pivot], rhs[bestRow]);

    for (int row = pivot + 1; row < 5; ++row)
      rhs[row] -= mFactoredCircuitMatrix[row][pivot] * rhs[pivot];
  }

  std::array<double, 5> solution{};
  for (int row = 4; row >= 0; --row)
  {
    double sum = rhs[row];
    for (int col = row + 1; col < 5; ++col)
      sum -= mFactoredCircuitMatrix[row][col] * solution[col];

    double divisor = mFactoredCircuitMatrix[row][row];
    if (std::abs(divisor) < kMinimumPivot)
      divisor = divisor < 0.0 ? -kMinimumPivot : kMinimumPivot;
    solution[row] = sum / divisor;
  }
  return solution;
}

double dsp::tone_stack::BasicNamToneStack::_ProcessCircuitSample(double input, int channel)
{
  auto& channelState = mCircuitStates[std::min(channel, static_cast<int>(mCircuitStates.size()) - 1)];
  double output = mCircuitB[0] * input;
  for (int i = 1; i <= kToneStackFilterOrder; ++i)
    output += mCircuitB[i] * channelState.x[i - 1] - mCircuitA[i] * channelState.y[i - 1];

  double previousOutput = 0.0;
  if (channelState.transitionSamplesRemaining > 0)
  {
    previousOutput = mPreviousCircuitB[0] * input;
    for (int i = 1; i <= kToneStackFilterOrder; ++i)
      previousOutput +=
        mPreviousCircuitB[i] * channelState.previousX[i - 1] - mPreviousCircuitA[i] * channelState.previousY[i - 1];
  }

  for (int i = kToneStackFilterOrder - 1; i > 0; --i)
  {
    channelState.x[i] = channelState.x[i - 1];
    channelState.y[i] = channelState.y[i - 1];
    if (channelState.transitionSamplesRemaining > 0)
    {
      channelState.previousX[i] = channelState.previousX[i - 1];
      channelState.previousY[i] = channelState.previousY[i - 1];
    }
  }
  channelState.x[0] = input;
  channelState.y[0] = output;

  if (channelState.transitionSamplesRemaining > 0)
  {
    channelState.previousX[0] = input;
    channelState.previousY[0] = previousOutput;
    const double fadeOut = static_cast<double>(channelState.transitionSamplesRemaining) / 128.0;
    --channelState.transitionSamplesRemaining;
    output = previousOutput * fadeOut + output * (1.0 - fadeOut);
  }

  return output;
}

DSP_SAMPLE** dsp::tone_stack::BasicNamToneStack::_ProcessCircuit(DSP_SAMPLE** inputs, const int numChannels,
                                                                 const int numFrames)
{
  const int channels = std::min(numChannels, 2);
  const int frames = std::min(numFrames, mMaxBlockSize);
  for (int ch = 0; ch < channels; ++ch)
  {
    for (int s = 0; s < frames; ++s)
      mOutputPointers[ch][s] = static_cast<DSP_SAMPLE>(_ProcessCircuitSample(inputs[ch][s], ch));
  }

  return mOutputPointers.data();
}
