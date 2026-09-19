#include <hal/hal.h>
#include <stackchan/motion/motion.h>

#include <cstdlib>
#include <iostream>
#include <memory>

namespace {

using stackchan::motion::Motion;
using stackchan::motion::Servo;

void expect(bool condition, const char* label)
{
    if (!condition) {
        std::cerr << label << '\n';
        std::exit(1);
    }
}

class RecordingServo : public Servo {
public:
    explicit RecordingServo(int initial_angle) : actual_angle(initial_angle)
    {
        set_angle_limit({-900, 900});
    }

    int actual_angle;
    int writes = 0;
    bool torque_enabled = false;

    int getCurrentAngle() override { return actual_angle; }
    bool getTorqueEnabled() override { return torque_enabled; }
    void setTorqueEnabled(bool enabled) override { torque_enabled = enabled; }

protected:
    void set_angle_impl(int angle) override
    {
        actual_angle = angle;
        ++writes;
    }
};

void advanceMotion(Motion& motion, int ticks = 100)
{
    for (int tick = 0; tick < ticks; ++tick) {
        GetHAL().now_ms += 20;
        motion.update();
    }
}

void testMotionPause()
{
    auto yaw = std::make_unique<RecordingServo>(100);
    auto pitch = std::make_unique<RecordingServo>(200);
    auto* yaw_servo = yaw.get();
    auto* pitch_servo = pitch.get();
    Motion motion(std::move(yaw), std::move(pitch));
    motion.init();
    motion.setAutoAngleSyncEnabled(false);
    motion.moveWithSpeed(500, 600, 500);
    advanceMotion(motion, 3);
    expect(motion.isMoving(), "Trajectory is active before pause");

    const auto held_angles = motion.getCurrentAngles();
    motion.setMotionPaused(true);
    expect(motion.isMotionPaused(), "Both axes can be paused");
    expect(motion.isModifyLocked(), "Modifiers see paused motion as locked");
    expect(!motion.isMoving(), "Pause cancels animation momentum immediately");
    expect(yaw_servo->actual_angle == held_angles.x && pitch_servo->actual_angle == held_angles.y,
           "Pause holds the measured position rather than the old target");
    const int yaw_writes = yaw_servo->writes;
    const int pitch_writes = pitch_servo->writes;
    motion.setMotionPaused(true);
    expect(yaw_servo->writes == yaw_writes && pitch_servo->writes == pitch_writes,
           "Repeated pause does not send additional position commands");

    motion.moveYaw(300);
    motion.movePitch(300);
    motion.moveYawWithSpeed(400, 300);
    motion.movePitchWithSpeed(400, 300);
    motion.move(-400, -400);
    motion.moveWithSpeed(400, 400, 500);
    motion.lookAtNormalized(0.7f, 0.7f);
    motion.lookAtPoint(1, 1, 1);
    motion.goHome();
    motion.yawServo().moveWithSpringParams(-500);
    motion.pitchServo().moveWithSpringParams(-500);
    motion.setModifyLock(false);
    expect(motion.isModifyLocked(), "Dizzy modifier unlock cannot clear the conversation pause");
    advanceMotion(motion);
    expect(yaw_servo->writes == yaw_writes && pitch_servo->writes == pitch_writes,
           "Position commands from all entry points are discarded while paused");

    yaw_servo->torque_enabled = true;
    pitch_servo->torque_enabled = true;
    advanceMotion(motion);
    expect(!yaw_servo->torque_enabled && !pitch_servo->torque_enabled,
           "Pause preserves automatic torque release safety");

    motion.setModifyLock(true);
    motion.setMotionPaused(false);
    expect(motion.isModifyLocked(), "Resuming preserves an independent modifier lock");
    motion.setModifyLock(false);
    expect(!motion.isMotionPaused() && !motion.isModifyLocked(), "Idle motion can resume");
    advanceMotion(motion);
    expect(yaw_servo->writes == yaw_writes && pitch_servo->writes == pitch_writes,
           "Resuming does not replay discarded commands or an interrupted trajectory");

    motion.moveWithSpeed(-200, -300, 500);
    advanceMotion(motion);
    expect(yaw_servo->actual_angle == -200 && pitch_servo->actual_angle == -300,
           "Fresh movements work after resuming");
}

}  // namespace

int main()
{
    testMotionPause();
    std::cout << "Motion pause tests passed\n";
}
