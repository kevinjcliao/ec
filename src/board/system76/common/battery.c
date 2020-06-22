#include <board/battery.h>
#include <board/smbus.h>
#include <common/config.h>
#include <common/debug.h>

#define BATTERY_ADDRESS 0x0B
#define CHARGER_ADDRESS 0x09

// ChargeOption0 flags
// Low Power Mode Enable
#define SBC_EN_LWPWR        ((uint16_t)(1 << 15))
// Watchdog Timer Adjust
#define SBC_WDTMR_ADJ_175S  ((uint16_t)(0b11 << 13))
// Switching Frequency
#define SBC_PWM_FREQ_800KHZ ((uint16_t)(0b01 << 8))
// IDCHG Amplifier Gain
#define SBC_IDCHC_GAIN      ((uint16_t)(1 << 3))

uint16_t battery_temp = 0;
uint16_t battery_voltage = 0;
uint16_t battery_current = 0;
uint16_t battery_charge = 0;
uint16_t battery_remaining_capacity = 0;
uint16_t battery_full_capacity = 0;
uint16_t battery_status = 0;
uint16_t battery_design_capacity = 0;
uint16_t battery_design_voltage = 0;

// Valid values are [0, 99].
// A value of 0 turns off the start threshold control.
config_t battery_start_threshold = {
    .config_id = {'B', 'C', 'T', 'L'},
    .config_short = "Battery Charging Start Threshold",
    .config_desc = "Relative capacity at which the battery will start charging",
    .value = {
        .min_value = 0,
        .max_value = 99,
        .value = 0, // Default to disabled
    },
    .set_callback = NULL,
};

// Valid values are [1, 100].
// A value of 100 turns off the stop threshold control.
config_t battery_stop_threshold = {
    .config_id = {'B', 'C', 'T', 'H'},
    .config_short = "Battery Charging Stop Threshold",
    .config_desc = "Relative capacity at which the battery will stop charging",
    .value = {
        .min_value = 1,
        .max_value = 100,
        .value = 100, // Default to disabled
    },
    .set_callback = NULL,
};

void battery_init(void) {
    config_register(&battery_start_threshold);
    config_register(&battery_stop_threshold);
}

int battery_get_start_threshold(void) {
    return config_get_value(&battery_start_threshold);
}

int battery_get_stop_threshold(void) {
    return config_get_value(&battery_stop_threshold);
}

bool battery_set_start_threshold(int32_t value) {
    return config_set_value(&battery_start_threshold, value);
}

bool battery_set_stop_threshold(int32_t value) {
    return config_set_value(&battery_stop_threshold, value);
}

// XXX: Assumption: ac_last is initialized high.
static bool charger_enabled = false;

int battery_charger_disable(void) {
    int res = 0;

    if (!charger_enabled) return res;

    // Set charge option 0 with 175s watchdog
    res = smbus_write(
        CHARGER_ADDRESS,
        0x12,
        SBC_EN_LWPWR |
        SBC_WDTMR_ADJ_175S |
        SBC_PWM_FREQ_800KHZ |
        SBC_IDCHC_GAIN
    );

    // Disable charge current
    res = smbus_write(CHARGER_ADDRESS, 0x14, 0);
    if (res < 0) return res;

    // Disable charge voltage
    res = smbus_write(CHARGER_ADDRESS, 0x15, 0);
    if (res < 0) return res;

    // Disable input current
    res = smbus_write(CHARGER_ADDRESS, 0x3F, 0);
    if (res < 0) return res;

    DEBUG("Charged disabled\n");
    charger_enabled = false;
    return 0;
}

int battery_charger_enable(void) {
    int res = 0;

    if (charger_enabled) return res;

    res = battery_charger_disable();
    if (res < 0) return res;

    // Set charge current in mA
    res = smbus_write(CHARGER_ADDRESS, 0x14, CHARGER_CHARGE_CURRENT);
    if (res < 0) return res;

    // Set charge voltage in mV
    res = smbus_write(CHARGER_ADDRESS, 0x15, CHARGER_CHARGE_VOLTAGE);
    if (res < 0) return res;

    // Set input current in mA
    res = smbus_write(CHARGER_ADDRESS, 0x3F, CHARGER_INPUT_CURRENT);
    if (res < 0) return res;

    // Set charge option 0 with watchdog disabled
    res = smbus_write(
        CHARGER_ADDRESS,
        0x12,
        SBC_EN_LWPWR |
        SBC_PWM_FREQ_800KHZ |
        SBC_IDCHC_GAIN
    );

    DEBUG("Charged enabled\n");
    charger_enabled = true;
    return 0;
}

/**
 * Configure the charger based on charging threshold values.
 */
int battery_charger_configure(void) {
    static bool should_charge = true;

    if (battery_get_stop_threshold() == 100) {
        // Stop threshold not configured: Always charge on AC.
        should_charge = true;
    }
    else if (battery_charge >= battery_get_stop_threshold()) {
        // Stop threshold configured: Stop charging at threshold.
        should_charge = false;
    }
    else if (battery_get_start_threshold() == 0) {
        // Start threshold not configured: Always charge up to stop threshold.
        should_charge = true;
    }
    else if (battery_charge <= battery_get_start_threshold()) {
        // Start threshold configured: Start charging at threshold.
        should_charge = true;
    }

    if (should_charge)
        return battery_charger_enable();
    return battery_charger_disable();
}

void battery_event(void) {
    int res = 0;

    #define command(N, V) { \
        res = smbus_read(BATTERY_ADDRESS, V, &N); \
        if (res < 0) { \
            N = 0; \
        } \
    }

    command(battery_temp, 0x08);
    command(battery_voltage, 0x09);
    command(battery_current, 0x0A);
    command(battery_charge, 0x0D);
    command(battery_remaining_capacity, 0x0F);
    command(battery_full_capacity, 0x10);
    command(battery_status, 0x16);
    command(battery_design_capacity, 0x18);
    command(battery_design_voltage, 0x19);

    #undef command
}

void battery_debug(void) {
    uint16_t data = 0;
    int res = 0;

    #define command(N, A, V) { \
        DEBUG("  " #N ": "); \
        res = smbus_read(A, V, &data); \
        if (res < 0) { \
            DEBUG("ERROR %04X\n", -res); \
        } else { \
            DEBUG("%04X\n", data); \
        } \
    }

    DEBUG("Battery:\n");
    command(Temperature, BATTERY_ADDRESS, 0x08);
    command(Voltage, BATTERY_ADDRESS, 0x09);
    command(Current, BATTERY_ADDRESS, 0x0A);
    command(Charge, BATTERY_ADDRESS, 0x0D);
    command(Status, BATTERY_ADDRESS, 0x16);

    DEBUG("Charger:\n");
    command(ChargeOption0, CHARGER_ADDRESS, 0x12);
    command(ChargeOption1, CHARGER_ADDRESS, 0x3B);
    command(ChargeOption2, CHARGER_ADDRESS, 0x38);
    command(ChargeOption3, CHARGER_ADDRESS, 0x37);
    command(ChargeCurrent, CHARGER_ADDRESS, 0x14);
    command(ChargeVoltage, CHARGER_ADDRESS, 0x15);
    command(DishargeCurrent, CHARGER_ADDRESS, 0x39);
    command(InputCurrent, CHARGER_ADDRESS, 0x3F);
    command(ProchotOption0, CHARGER_ADDRESS, 0x3C);
    command(ProchotOption1, CHARGER_ADDRESS, 0x3D);
    command(ProchotStatus, CHARGER_ADDRESS, 0x3A);

    #undef command
}
