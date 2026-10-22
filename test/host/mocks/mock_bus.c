/* Host-test mock: scripted I2C bus (see mock_bus.h). */
#include "mock_bus.h"

#include <string.h>

void mock_bus_reset(mock_bus_t *b)
{
    memset(b, 0, sizeof(*b));
    b->latency_ms_on_timeout = 50;
}

static void log_op(mock_bus_t *b, char kind, uint8_t addr, uint8_t reg, uint8_t val, size_t n)
{
    if (b->n_ops < MOCK_BUS_MAX_LOG) {
        b->log[b->n_ops].kind = kind;
        b->log[b->n_ops].addr = addr;
        b->log[b->n_ops].reg = reg;
        b->log[b->n_ops].val = val;
        b->log[b->n_ops].n = n;
    }
    b->n_ops++;
}

/* Result of the transaction that is about to be issued. */
static esp_err_t scripted(mock_bus_t *b, size_t idx)
{
    esp_err_t r = ESP_OK;
    if (b->fail_all != 0) {
        r = b->fail_all;
    } else if (idx < b->script_len) {
        r = b->script[idx];
    }
    if (r == ESP_ERR_ACCEL_TIMEOUT) {
        b->now_us += (int64_t)b->latency_ms_on_timeout * 1000;
    }
    return r;
}

/* The register file answering at addr, or NULL if nothing ACKs there. */
static uint8_t *regs_for(mock_bus_t *b, uint8_t addr)
{
    if (addr != 0 && addr == b->present_addr) {
        return b->regs;
    }
    if (addr != 0 && addr == b->wrong_addr) {
        return b->wrong_regs;
    }
    return NULL;
}

static esp_err_t m_probe(void *bus, uint8_t addr, uint32_t timeout_ms)
{
    (void)timeout_ms;
    mock_bus_t *b = (mock_bus_t *)bus;
    size_t idx = b->n_ops;
    log_op(b, 'P', addr, 0, 0, 0);
    esp_err_t r = scripted(b, idx);
    if (r != ESP_OK) {
        return r;
    }
    return (regs_for(b, addr) != NULL) ? ESP_OK : ESP_ERR_ACCEL_NOT_PRESENT;
}

static esp_err_t m_write(void *bus, uint8_t addr, uint8_t reg, uint8_t val, uint32_t timeout_ms)
{
    (void)timeout_ms;
    mock_bus_t *b = (mock_bus_t *)bus;
    size_t idx = b->n_ops;
    log_op(b, 'W', addr, reg, val, 1);
    b->n_writes++;
    esp_err_t r = scripted(b, idx);
    if (r != ESP_OK) {
        return r;
    }
    uint8_t *regs = regs_for(b, addr);
    if (regs == NULL) {
        return ESP_ERR_ACCEL_NOT_PRESENT;
    }
    regs[reg] = val;
    return ESP_OK;
}

static esp_err_t m_read(void *bus, uint8_t addr, uint8_t reg, uint8_t *buf, size_t n, uint32_t timeout_ms)
{
    (void)timeout_ms;
    mock_bus_t *b = (mock_bus_t *)bus;
    size_t idx = b->n_ops;
    log_op(b, 'R', addr, reg, 0, n);
    esp_err_t r = scripted(b, idx);
    if (r != ESP_OK) {
        return r;
    }
    uint8_t *regs = regs_for(b, addr);
    if (regs == NULL) {
        return ESP_ERR_ACCEL_NOT_PRESENT;
    }
    for (size_t i = 0; i < n; i++) {
        buf[i] = regs[(uint8_t)(reg + i)];
    }
    return ESP_OK;
}

static esp_err_t m_recover(void *bus)
{
    mock_bus_t *b = (mock_bus_t *)bus;
    log_op(b, 'X', 0, 0, 0, 0);
    b->n_recovers++;
    return ESP_OK;
}

const accel_bus_ops_t mock_bus_ops = {
    .probe = m_probe,
    .write_reg = m_write,
    .read_regs = m_read,
    .recover = m_recover,
};
