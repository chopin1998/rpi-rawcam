#include "i2c_ctrl.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

void i2c_ctrl_init(I2cCtrl *c) {
    c->fd = -1;
    c->addr = 0x3b;
    pthread_mutex_init(&c->mu, NULL);
    c->mu_init = true;
}

bool i2c_ctrl_open(I2cCtrl *c, const char *dev_path, uint16_t addr) {
    pthread_mutex_lock(&c->mu);
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    c->fd = open(dev_path, O_RDWR);
    c->addr = addr;
    bool ok = (c->fd >= 0);
    pthread_mutex_unlock(&c->mu);
    return ok;
}

void i2c_ctrl_close(I2cCtrl *c) {
    if (!c->mu_init) return;
    pthread_mutex_lock(&c->mu);
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    pthread_mutex_unlock(&c->mu);
}

bool i2c_ctrl_read_u32(I2cCtrl *c, uint16_t reg_addr, uint32_t *out) {
    pthread_mutex_lock(&c->mu);
    if (c->fd < 0) {
        pthread_mutex_unlock(&c->mu);
        return false;
    }

    uint8_t wbuf[2] = { (uint8_t)(reg_addr >> 8), (uint8_t)(reg_addr & 0xFF) };
    uint8_t rbuf[4] = { 0 };

    struct i2c_msg msgs[2];
    memset(msgs, 0, sizeof(msgs));
    msgs[0].addr  = c->addr;
    msgs[0].flags = 0;
    msgs[0].len   = 2;
    msgs[0].buf   = wbuf;
    msgs[1].addr  = c->addr;
    msgs[1].flags = I2C_M_RD;
    msgs[1].len   = 4;
    msgs[1].buf   = rbuf;

    struct i2c_rdwr_ioctl_data data;
    data.msgs  = msgs;
    data.nmsgs = 2;

    int ret = ioctl(c->fd, I2C_RDWR, &data);
    pthread_mutex_unlock(&c->mu);
    if (ret != 2) return false;

    uint32_t net_val = 0;
    memcpy(&net_val, rbuf, 4);
    *out = ntohl(net_val);
    return true;
}

bool i2c_ctrl_write_u32(I2cCtrl *c, uint16_t reg_addr, uint32_t val) {
    pthread_mutex_lock(&c->mu);
    if (c->fd < 0) {
        pthread_mutex_unlock(&c->mu);
        return false;
    }

    uint8_t buf[6];
    buf[0] = (uint8_t)(reg_addr >> 8);
    buf[1] = (uint8_t)(reg_addr & 0xFF);
    uint32_t net_val = htonl(val);
    memcpy(buf + 2, &net_val, 4);

    struct i2c_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.addr  = c->addr;
    msg.flags = 0;
    msg.len   = 6;
    msg.buf   = buf;

    struct i2c_rdwr_ioctl_data data;
    data.msgs  = &msg;
    data.nmsgs = 1;

    int ret = ioctl(c->fd, I2C_RDWR, &data);
    pthread_mutex_unlock(&c->mu);
    return ret == 1;
}
