#ifndef RAWCAM_I2C_CTRL_H
#define RAWCAM_I2C_CTRL_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

/* VEYE MIPI RAW camera register map (from veye_mvcam.h) */
#define REG_MODEL_NAME      0x0004
#define REG_FORMAT_CAP      0x0028
#define REG_PIXEL_FORMAT    0x0804
#define REG_SENSOR_WIDTH    0x0808
#define REG_SENSOR_HEIGHT   0x080C
#define REG_MAX_FRAME_RATE  0x0810 /* 0.01 fps */
#define REG_FRAMERATE       0x0814 /* 0.01 fps */
#define REG_ROI_WIDTH       0x0818
#define REG_ROI_HEIGHT      0x081C
#define REG_ROI_OFFSET_X    0x0820
#define REG_ROI_OFFSET_Y    0x0824
#define REG_MIN_ROI_WIDTH   0x0844
#define REG_MIN_ROI_HEIGHT  0x0848

#define REG_EXPOSURE_MODE   0x0C04 /* 0=Auto, 1=Once, 2=Manual */
#define REG_ME_TIME         0x0C10 /* Manual exposure (us) */
#define REG_AE_MAX_TIME     0x0C14 /* Auto exposure max (us) */
#define REG_EXP_TIME        0x0C18 /* Actual hardware exposure (us) */
#define REG_GAIN_MODE       0x0C1C /* 0=Auto, 1=Once, 2=Manual */
#define REG_MANUAL_GAIN     0x0C20 /* Manual gain (0.1 dB) */
#define REG_AG_MAX_GAIN     0x0C24 /* Auto gain max (0.1 dB) */
#define REG_CUR_GAIN        0x0C28 /* Actual hardware gain (0.1 dB) */

typedef struct {
    int             fd;
    uint16_t        addr;
    pthread_mutex_t mu;
    bool            mu_init;
} I2cCtrl;

void i2c_ctrl_init(I2cCtrl *c);
bool i2c_ctrl_open(I2cCtrl *c, const char *dev_path, uint16_t addr);
void i2c_ctrl_close(I2cCtrl *c);
bool i2c_ctrl_read_u32(I2cCtrl *c, uint16_t reg_addr, uint32_t *out);
bool i2c_ctrl_write_u32(I2cCtrl *c, uint16_t reg_addr, uint32_t val);

#endif /* RAWCAM_I2C_CTRL_H */
