"""Fault-injection checks for the real BSP feedback reader and motion startup.

python tools/motion_test/run.py --compare-v240
Requires installed PlatformIO dependencies and Python's ziglang package.
Only mocked servo I/O is used; this does not move connected hardware.
"""

import argparse
import importlib.util
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
CACHE = ROOT / ".pio" / "motion-test"
spec = importlib.util.spec_from_file_location("bsp_patch", ROOT / "tools/patch_stackchan_bsp.py")
patch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch)


def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


HARNESS = r'''
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
void check(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
uint32_t tick = 100;
uint32_t millis() { return tick; }
void delay(int) {}
int warnings = 0;
#define ESP_LOGW(...) (++warnings)
struct Vec { int x; int y; };
struct Bus { int pos = -1; int ReadPos(int) { return pos; } } _scs_bus;
namespace uitk_intl {
int clamp(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }
}
struct Servo {
    int estimate = 17;
    virtual int getCurrentAngle() { return estimate; }
    Vec getAngleLimit() { return {-1280,1280}; }
};
struct ScsServo : Servo {
    struct { int id = 1; Vec rawPosLimit{0,1000}; } _config;
    int _zero_pos = 460;
    uint32_t _feedback_error_at = 0;
    bool _feedback_error_logged = false;
    @FEEDBACK@
};
enum class BodyMotionState { Stopped, Idle, Joy };
BodyMotionState bodyMotionState = BodyMotionState::Stopped;
bool bodyMotionSkipAutoStart = false;
bool levelHoldActive = false;
int bodyMotionYawBase = 0, bodyMotionPitchBase = 0;
int bodyMotionYawTarget = 0, bodyMotionPitchTarget = 0;
uint32_t bodyMotionStartedAt = 0, bodyMotionLastUpdateAt = 0;
constexpr int BODY_IDLE_SERVO_SPEED = 160;
struct Calibration {
    struct Data { bool servoValid = true; } value;
    const Data& data() { return value; }
} calibrationController;
bool allowedMode = true;
bool modeUsesBodyMotion() { return allowedMode; }
int bodyHomeYaw() { return 0; }
int bodyHomePitch() { return 200; }
int clampBodyYaw(int v) { return uitk_intl::clamp(v, -1280,1280); }
int clampBodyPitch(int v) { return uitk_intl::clamp(v, 0,900); }
struct SerialMock { template<typename... T> void printf(const char*,T...) {} } Serial;
struct Hardware {
    int powerCalls = 0;
    void setServoPowerEnabled(bool) { ++powerCalls; }
    struct MotionMock {
        int reads = 0, moves = 0, torqueCalls = 0, syncCalls = 0;
        bool sync = true;
        Vec position{120,180}, target{};
        Vec getCurrentAngles() { ++reads; return position; }
        void setAutoAngleSyncEnabled(bool enabled) { ++syncCalls; sync = enabled; }
        void setAutoTorqueReleaseEnabled(bool) {}
        void setTorqueEnabled(bool) { ++torqueCalls; }
        void move(int yaw,int pitch,int) { ++moves; target={yaw,pitch}; }
    } Motion;
} M5StackChan;
@STARTUP@

int main() {
    try {
        ScsServo servo;
        _scs_bus.pos = -1;
        check(servo.getCurrentAngle() == 17, "timeout became a full-left angle");
        _scs_bus.pos = 524;
        check(servo.getCurrentAngle() == 200, "valid feedback conversion changed");
        servo.estimate = 190;
        _scs_bus.pos = -1;
        check(servo.getCurrentAngle() == 190, "timeout discarded current trajectory");
        _scs_bus.pos = 65535;
        check(servo.getCurrentAngle() == 190, "out-of-range feedback accepted");
        check(warnings == 1, "feedback warnings were not throttled");
        tick += 5000;
        check(servo.getCurrentAngle() == 190 && warnings == 2, "warning throttle did not recover");
        _scs_bus.pos = 0;
        check(servo.getCurrentAngle() == -1280, "valid physical left endpoint rejected");
        _scs_bus.pos = 1000;
        check(servo.getCurrentAngle() == 1280, "valid physical right endpoint rejected");

        check(startBodyMotion(), "cold startup failed");
        auto& motion = M5StackChan.Motion;
        check(motion.reads == 1 && motion.moves == 1 && motion.target.x == 120 &&
              motion.target.y == 180, "startup did not hold measured position");
        check(!motion.sync, "continuous motion still resyncs UART feedback");
        bodyMotionYawTarget = 8;
        bodyMotionPitchTarget = 193;
        const uint32_t started = bodyMotionStartedAt;
        for (auto state : {BodyMotionState::Idle, BodyMotionState::Joy}) {
            bodyMotionState = state;
            motion.position = {-1280,0};  // Must never be read on mode change.
            tick += 100;
            check(startBodyMotion(), "mode transition failed");
            check(motion.reads == 1 && motion.moves == 1 && motion.torqueCalls == 1 &&
                  M5StackChan.powerCalls == 1, "mode change restarted active servos");
            check(bodyMotionState == state && bodyMotionYawTarget == 8 &&
                  bodyMotionPitchTarget == 193 && bodyMotionStartedAt == started,
                  "mode change discarded the current trajectory");
        }
        bodyMotionState = BodyMotionState::Stopped;
        bodyMotionSkipAutoStart = true;
        check(!startBodyMotion() && motion.moves == 1, "auto-start skip ignored");
        calibrationController.value.servoValid = false;
        check(!startBodyMotion(true), "uncalibrated servo started");
        std::puts("PASS: invalid feedback, endpoints, warning throttle, continuous startup, MQTT transition");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
'''


def run_case(name, bsp, main, should_pass=True):
    source = HARNESS.replace("@FEEDBACK@", function(bsp, "    int getCurrentAngle() override"))
    source = source.replace("@STARTUP@", function(main, "bool startBodyMotion("))
    path = CACHE / f"{name}.cpp"
    exe = CACHE / f"{name}.exe"
    path.write_text(source, encoding="utf-8")
    subprocess.run([sys.executable, "-m", "ziglang", "c++", "-std=c++17", "-O1", str(path), "-o", str(exe)], check=True)
    result = subprocess.run([str(exe)])
    if (result.returncode == 0) != should_pass:
        raise SystemExit(f"Unexpected result: {name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compare-v240", action="store_true")
    args = parser.parse_args()
    CACHE.mkdir(parents=True, exist_ok=True)
    bsp = (ROOT / ".pio/libdeps/m5stack-cores3/StackChan-BSP/src/M5StackChan.cpp").read_text(encoding="utf-8")
    original = bsp.replace(patch.PATCHED, patch.ORIGINAL).replace(patch.MEMBERS_PATCHED, patch.MEMBERS_ORIGINAL)
    patched = patch.patch_source(original)
    assert patch.patch_source(patched) == patched, "patch is not idempotent"
    try:
        patch.patch_source(original.replace(patch.ORIGINAL, "unexpected upstream code"))
    except RuntimeError:
        pass
    else:
        raise AssertionError("unsupported dependency source accepted")
    if args.compare_v240:
        old_main = subprocess.check_output(["git", "show", "5d7a6b5:src/main.cpp"], cwd=ROOT).decode("utf-8")
        run_case("v240", original, old_main, should_pass=False)
        print("Confirmed: v2.4.0 reproduces the left-jump regression", flush=True)
    run_case("fixed", patched, (ROOT / "src/main.cpp").read_text(encoding="utf-8"))


if __name__ == "__main__":
    main()
