#include "input.h"

#include <string.h>

static HidSixAxisSensorHandle six_handheld, six_fullkey, six_joydual[2];
static HidVibrationDeviceHandle vib_handheld[2], vib_joydual[2], vib_fullkey[2];

void input_init(void) {
    hidInitializeTouchScreen();

    hidGetSixAxisSensorHandles(&six_handheld, 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
    hidGetSixAxisSensorHandles(&six_fullkey, 1, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey);
    hidGetSixAxisSensorHandles(six_joydual, 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual);
    hidStartSixAxisSensor(six_handheld);
    hidStartSixAxisSensor(six_fullkey);
    hidStartSixAxisSensor(six_joydual[0]);
    hidStartSixAxisSensor(six_joydual[1]);

    hidInitializeVibrationDevices(vib_handheld, 2, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
    hidInitializeVibrationDevices(vib_joydual, 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual);
    hidInitializeVibrationDevices(vib_fullkey, 2, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey);
}

void input_exit(void) {
    hidStopSixAxisSensor(six_handheld);
    hidStopSixAxisSensor(six_fullkey);
    hidStopSixAxisSensor(six_joydual[0]);
    hidStopSixAxisSensor(six_joydual[1]);
}

static const struct { u64 nx; u32 eb; } kButtonMap[] = {
    {HidNpadButton_A, EB_BTN_A},         {HidNpadButton_B, EB_BTN_B},
    {HidNpadButton_X, EB_BTN_X},         {HidNpadButton_Y, EB_BTN_Y},
    {HidNpadButton_StickL, EB_BTN_LSTICK}, {HidNpadButton_StickR, EB_BTN_RSTICK},
    {HidNpadButton_L, EB_BTN_L},         {HidNpadButton_R, EB_BTN_R},
    {HidNpadButton_ZL, EB_BTN_ZL},       {HidNpadButton_ZR, EB_BTN_ZR},
    {HidNpadButton_Plus, EB_BTN_PLUS},   {HidNpadButton_Minus, EB_BTN_MINUS},
    {HidNpadButton_Left, EB_BTN_LEFT},   {HidNpadButton_Up, EB_BTN_UP},
    {HidNpadButton_Right, EB_BTN_RIGHT}, {HidNpadButton_Down, EB_BTN_DOWN},
    {HidNpadButton_LeftSL | HidNpadButton_RightSL, EB_BTN_SL},
    {HidNpadButton_LeftSR | HidNpadButton_RightSR, EB_BTN_SR},
};

static int16_t clamp_stick(s32 v) {
    if (v > 32767) return 32767;
    if (v < -32767) return -32767;
    return (int16_t)v;
}

void input_read(PadState* pad, eb_input_t* out) {
    memset(out, 0, sizeof *out);
    const u64 held = padGetButtons(pad);
    for (size_t i = 0; i < sizeof kButtonMap / sizeof kButtonMap[0]; i++)
        if (held & kButtonMap[i].nx) out->buttons |= kButtonMap[i].eb;

    /* The HOME button is reserved by the system. Send it with the
     * combo  "-" + D-pad Down  instead. */
    if ((held & HidNpadButton_Minus) && (held & HidNpadButton_Down)) {
        out->buttons &= ~(EB_BTN_MINUS | EB_BTN_DOWN);
        out->buttons |= EB_BTN_HOME;
    }

    HidAnalogStickState l = padGetStickPos(pad, 0), r = padGetStickPos(pad, 1);
    out->lx = clamp_stick(l.x);
    out->ly = clamp_stick(l.y);
    out->rx = clamp_stick(r.x);
    out->ry = clamp_stick(r.y);

    const u32 style = padGetStyleSet(pad);
    HidSixAxisSensorHandle six = six_fullkey;
    if (style & HidNpadStyleTag_NpadHandheld) {
        out->controller_type = EB_CTRL_HANDHELD;
        six = six_handheld;
    } else if (style & HidNpadStyleTag_NpadJoyDual) {
        out->controller_type = EB_CTRL_DUAL_JOYCON;
        six = six_joydual[1];   /* right Joy-Con: the one games aim with */
    } else {
        out->controller_type = EB_CTRL_PRO;
    }
    HidSixAxisSensorState ss = {0};
    if (hidGetSixAxisSensorStates(six, &ss, 1)) {
        out->gyro[0] = ss.angular_velocity.x;
        out->gyro[1] = ss.angular_velocity.y;
        out->gyro[2] = ss.angular_velocity.z;
        out->accel[0] = ss.acceleration.x;
        out->accel[1] = ss.acceleration.y;
        out->accel[2] = ss.acceleration.z;
    }

    HidTouchScreenState ts = {0};
    if (hidGetTouchScreenStates(&ts, 1)) {
        out->touch_count = ts.count > EB_MAX_TOUCH ? EB_MAX_TOUCH : (uint8_t)ts.count;
        for (int i = 0; i < out->touch_count; i++) {
            out->touch[i].x = (uint16_t)ts.touches[i].x;
            out->touch[i].y = (uint16_t)ts.touches[i].y;
            out->touch[i].id = (uint8_t)ts.touches[i].finger_id;
        }
    }
}

void input_rumble(PadState* pad, const eb_rumble_t* r) {
    HidVibrationValue v[2];
    for (int i = 0; i < 2; i++) {
        v[i].amp_low = r->amp_low;
        v[i].freq_low = r->freq_low > 0 ? r->freq_low : 160.0f;
        v[i].amp_high = r->amp_high;
        v[i].freq_high = r->freq_high > 0 ? r->freq_high : 320.0f;
    }
    const u32 style = padGetStyleSet(pad);
    if (style & HidNpadStyleTag_NpadHandheld) hidSendVibrationValues(vib_handheld, v, 2);
    else if (style & HidNpadStyleTag_NpadJoyDual) hidSendVibrationValues(vib_joydual, v, 2);
    else hidSendVibrationValues(vib_fullkey, v, 2);
}
