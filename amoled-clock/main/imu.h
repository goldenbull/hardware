/*
 * QMI8658 六轴 IMU，这里只用加速度计判断重力方向，用来自动翻转屏幕。
 * 陀螺仪全程不开，省电也省事。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* 屏幕翻转状态变化时回调，flipped=true 表示需要转 180° */
typedef void (*imu_orientation_cb_t)(bool flipped);

/*
 * 探测 IMU 并启动方向监测任务。
 * 传感器不在（比如不带 IMU 的板子）时返回错误，调用方可以忽略，
 * 只是没有自动翻转而已。
 */
esp_err_t imu_init(imu_orientation_cb_t on_change);

/* 读一次加速度，单位 g。调试接线方向时用得上 */
esp_err_t imu_read_accel(float *ax, float *ay, float *az);
