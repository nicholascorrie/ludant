#pragma once

#include <cstdint>

namespace ludant {

struct BME280Calibration {
    uint16_t dig_t1{0};
    int16_t dig_t2{0};
    int16_t dig_t3{0};
    uint16_t dig_p1{0};
    int16_t dig_p2{0};
    int16_t dig_p3{0};
    int16_t dig_p4{0};
    int16_t dig_p5{0};
    int16_t dig_p6{0};
    int16_t dig_p7{0};
    int16_t dig_p8{0};
    int16_t dig_p9{0};
    uint8_t dig_h1{0};
    int16_t dig_h2{0};
    uint8_t dig_h3{0};
    int16_t dig_h4{0};
    int16_t dig_h5{0};
    int8_t dig_h6{0};
};

// Convert the raw ADC registers from a BME280 into degrees Celsius, percent
// relative humidity, and hectopascals. BME280 temperature/pressure/humidity
// registers are not display-ready values; the factory calibration registers
// must be applied for every sample.
inline bool compensateBME280(const BME280Calibration& calibration,
                             int32_t raw_pressure,
                             int32_t raw_temperature,
                             int32_t raw_humidity,
                             double& temperature_c,
                             double& humidity_percent,
                             double& pressure_hpa) {
    if (calibration.dig_t1 == 0 || calibration.dig_p1 == 0) return false;

    const double temperature_part = static_cast<double>(raw_temperature >> 3) -
                                    static_cast<double>(calibration.dig_t1) * 2.0;
    const double temperature_part_squared =
        (static_cast<double>(raw_temperature >> 4) - calibration.dig_t1) *
        (static_cast<double>(raw_temperature >> 4) - calibration.dig_t1);
    const double var1 = temperature_part * calibration.dig_t2 / 2048.0;
    const double var2 = temperature_part_squared * calibration.dig_t3 / 16384.0 / 4096.0;
    const double fine_temperature = var1 + var2;
    temperature_c = (fine_temperature * 5.0 + 128.0) / 256.0 / 100.0;

    double pressure_var1 = fine_temperature / 2.0 - 64000.0;
    double pressure_var2 = pressure_var1 * pressure_var1 * calibration.dig_p6 / 32768.0;
    pressure_var2 += pressure_var1 * calibration.dig_p5 * 2.0;
    pressure_var2 = pressure_var2 / 4.0 + calibration.dig_p4 * 65536.0;
    pressure_var1 = (calibration.dig_p3 * pressure_var1 * pressure_var1 / 524288.0 +
                     calibration.dig_p2 * pressure_var1) / 524288.0;
    pressure_var1 = (1.0 + pressure_var1 / 32768.0) * calibration.dig_p1;
    if (pressure_var1 == 0.0) return false;
    double pressure = (1048576.0 - raw_pressure - pressure_var2 / 4096.0) *
                      6250.0 / pressure_var1;
    pressure_var1 = calibration.dig_p9 * pressure * pressure / 2147483648.0;
    pressure_var2 = pressure * calibration.dig_p8 / 32768.0;
    pressure += (pressure_var1 + pressure_var2 + calibration.dig_p7) / 16.0;
    pressure_hpa = pressure / 100.0;

    double humidity = fine_temperature - 76800.0;
    humidity = (raw_humidity - (calibration.dig_h4 * 64.0 + calibration.dig_h5 / 16384.0 * humidity)) *
               (calibration.dig_h2 / 65536.0 *
                (1.0 + calibration.dig_h6 / 67108864.0 * humidity *
                 (1.0 + calibration.dig_h3 / 67108864.0 * humidity)));
    humidity *= 1.0 - calibration.dig_h1 * humidity / 524288.0;
    if (humidity < 0.0) humidity = 0.0;
    if (humidity > 100.0) humidity = 100.0;
    humidity_percent = humidity;
    return true;
}

} // namespace ludant
