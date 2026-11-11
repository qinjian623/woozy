// The Computer Language Benchmarks Game
// https://salsa.debian.org/benchmarksgame-team/benchmarksgame/
//
// contributed by Jeffrey Beu
//
// Single-threaded comparison reference for nbody.wz.  Upstream nbody/1.cpp at
// commit ad18b203dd1769724f4eea94fc3ac1e99f6593e0 expresses the five bodies
// as a variadic tuple of value structs; this file deliberately uses the same
// scalar loop/array algorithm as the Woozy program instead of that template
// machinery, so the measured work is identical.
//
// Representation: one flat std::vector<double> with the per-body layout
// [x, y, z, vx, vy, vz, mass] (7 doubles per body, 35 values).  Both sides
// run the same 10 i<j pair updates per step, advance positions with the same
// dt, and evaluate energy in the same order (kinetic for body i, then its
// potentials with every j > i).
//
// Interface: one runtime decimal argument via strtoll.  Two stdout lines:
// initial and final energy printed with "%.17g"; the runner compares them
// numerically.  Invalid arguments print to stderr and exit 2.  Neither this
// file nor nbody.wz enables fast-math or noalias.
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

const double PI = 3.141592653589793;
const double SOLAR_MASS = 4.0 * PI * PI;
const double DAYS_PER_YEAR = 365.24;
const double DT = 0.01;

const std::int64_t BODIES = 5;
const std::int64_t STRIDE = 7;
const std::int64_t PX = 0;
const std::int64_t PY = 1;
const std::int64_t PZ = 2;
const std::int64_t VX = 3;
const std::int64_t VY = 4;
const std::int64_t VZ = 5;
const std::int64_t MASS = 6;

void put(std::vector<double> &b, std::int64_t index, double x, double y,
         double z, double vx, double vy, double vz, double mass) {
    const std::int64_t base = index * STRIDE;
    b[static_cast<std::size_t>(base + PX)] = x;
    b[static_cast<std::size_t>(base + PY)] = y;
    b[static_cast<std::size_t>(base + PZ)] = z;
    b[static_cast<std::size_t>(base + VX)] = vx;
    b[static_cast<std::size_t>(base + VY)] = vy;
    b[static_cast<std::size_t>(base + VZ)] = vz;
    b[static_cast<std::size_t>(base + MASS)] = mass;
}

// Sun, Jupiter, Saturn, Uranus, Neptune with the upstream constants.
std::vector<double> bodies() {
    std::vector<double> b(static_cast<std::size_t>(BODIES * STRIDE));
    put(b, 0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, SOLAR_MASS);
    put(b, 1, 4.84143144246472090e+00, -1.16032004402742839e+00,
        -1.03622044471123109e-01, 1.66007664274403694e-03 * DAYS_PER_YEAR,
        7.69901118419740425e-03 * DAYS_PER_YEAR,
        -6.90460016972063023e-05 * DAYS_PER_YEAR,
        9.54791938424326609e-04 * SOLAR_MASS);
    put(b, 2, 8.34336671824457987e+00, 4.12479856412430479e+00,
        -4.03523417114321381e-01, -2.76742510726862411e-03 * DAYS_PER_YEAR,
        4.99852801234917238e-03 * DAYS_PER_YEAR,
        2.30417297573763929e-05 * DAYS_PER_YEAR,
        2.85885980666130812e-04 * SOLAR_MASS);
    put(b, 3, 1.28943695621391310e+01, -1.51111514016986312e+01,
        -2.23307578892655734e-01, 2.96460137564761618e-03 * DAYS_PER_YEAR,
        2.37847173959480950e-03 * DAYS_PER_YEAR,
        -2.96589568540237556e-05 * DAYS_PER_YEAR,
        4.36624404335156298e-05 * SOLAR_MASS);
    put(b, 4, 1.53796971148509165e+01, -2.59193146099879641e+01,
        1.79258772950371181e-01, 2.68067772490389322e-03 * DAYS_PER_YEAR,
        1.62824170038242295e-03 * DAYS_PER_YEAR,
        -9.51592254519715870e-05 * DAYS_PER_YEAR,
        5.15138902046611451e-05 * SOLAR_MASS);
    return b;
}

// Same total momentum offset as the upstream `offset` visitor.
void offset(std::vector<double> &b) {
    double px = 0.0, py = 0.0, pz = 0.0;
    for (std::int64_t i = 0; i < BODIES; ++i) {
        const std::int64_t base = i * STRIDE;
        px += b[static_cast<std::size_t>(base + VX)] * b[static_cast<std::size_t>(base + MASS)];
        py += b[static_cast<std::size_t>(base + VY)] * b[static_cast<std::size_t>(base + MASS)];
        pz += b[static_cast<std::size_t>(base + VZ)] * b[static_cast<std::size_t>(base + MASS)];
    }
    b[static_cast<std::size_t>(VX)] = -px / SOLAR_MASS;
    b[static_cast<std::size_t>(VY)] = -py / SOLAR_MASS;
    b[static_cast<std::size_t>(VZ)] = -pz / SOLAR_MASS;
}

// Upstream energy order: kinetic term for body i, then one potential term for
// every j > i.
double energy(const std::vector<double> &b) {
    double e = 0.0;
    for (std::int64_t i = 0; i < BODIES; ++i) {
        const std::int64_t ib = i * STRIDE;
        e += 0.5 * b[static_cast<std::size_t>(ib + MASS)] *
             (b[static_cast<std::size_t>(ib + VX)] * b[static_cast<std::size_t>(ib + VX)] +
              b[static_cast<std::size_t>(ib + VY)] * b[static_cast<std::size_t>(ib + VY)] +
              b[static_cast<std::size_t>(ib + VZ)] * b[static_cast<std::size_t>(ib + VZ)]);
        for (std::int64_t j = i + 1; j < BODIES; ++j) {
            const std::int64_t jb = j * STRIDE;
            const double dx = b[static_cast<std::size_t>(ib + PX)] - b[static_cast<std::size_t>(jb + PX)];
            const double dy = b[static_cast<std::size_t>(ib + PY)] - b[static_cast<std::size_t>(jb + PY)];
            const double dz = b[static_cast<std::size_t>(ib + PZ)] - b[static_cast<std::size_t>(jb + PZ)];
            const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            e -= (b[static_cast<std::size_t>(ib + MASS)] *
                  b[static_cast<std::size_t>(jb + MASS)]) / distance;
        }
    }
    return e;
}

// One upstream `advance`: all 10 i<j pair velocity updates, then all
// positions advanced by dt * velocity.
void advance(std::vector<double> &b) {
    for (std::int64_t i = 0; i < BODIES; ++i) {
        const std::int64_t ib = i * STRIDE;
        for (std::int64_t j = i + 1; j < BODIES; ++j) {
            const std::int64_t jb = j * STRIDE;
            const double dx = b[static_cast<std::size_t>(ib + PX)] - b[static_cast<std::size_t>(jb + PX)];
            const double dy = b[static_cast<std::size_t>(ib + PY)] - b[static_cast<std::size_t>(jb + PY)];
            const double dz = b[static_cast<std::size_t>(ib + PZ)] - b[static_cast<std::size_t>(jb + PZ)];
            const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            const double mag = DT / (distance * distance * distance);
            b[static_cast<std::size_t>(ib + VX)] -= dx * b[static_cast<std::size_t>(jb + MASS)] * mag;
            b[static_cast<std::size_t>(ib + VY)] -= dy * b[static_cast<std::size_t>(jb + MASS)] * mag;
            b[static_cast<std::size_t>(ib + VZ)] -= dz * b[static_cast<std::size_t>(jb + MASS)] * mag;
            b[static_cast<std::size_t>(jb + VX)] += dx * b[static_cast<std::size_t>(ib + MASS)] * mag;
            b[static_cast<std::size_t>(jb + VY)] += dy * b[static_cast<std::size_t>(ib + MASS)] * mag;
            b[static_cast<std::size_t>(jb + VZ)] += dz * b[static_cast<std::size_t>(ib + MASS)] * mag;
        }
    }
    for (std::int64_t i = 0; i < BODIES; ++i) {
        const std::int64_t base = i * STRIDE;
        b[static_cast<std::size_t>(base + PX)] +=
            DT * b[static_cast<std::size_t>(base + VX)];
        b[static_cast<std::size_t>(base + PY)] +=
            DT * b[static_cast<std::size_t>(base + VY)];
        b[static_cast<std::size_t>(base + PZ)] +=
            DT * b[static_cast<std::size_t>(base + VZ)];
    }
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: nbody <n>\n");
        return 2;
    }
    errno = 0;
    char *end = nullptr;
    const long long parsed = std::strtoll(argv[1], &end, 10);
    if (errno != 0 || end == argv[1] || *end != '\0' || parsed < 0) {
        std::fprintf(stderr, "nbody: n must be a non-negative integer\n");
        return 2;
    }

    std::vector<double> system = bodies();
    offset(system);
    std::printf("%.17g\n", energy(system));
    for (long long step = 0; step < parsed; ++step) {
        advance(system);
    }
    std::printf("%.17g\n", energy(system));
    return 0;
}
