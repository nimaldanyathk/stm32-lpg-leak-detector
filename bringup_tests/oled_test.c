/*
 * OLED test - Black Pill + 0.96" SSD1306 (I2C 0x3C)
 * Copyright (c) 2026 Nimal Danyath K (nimaldanyathk). All rights reserved.
 *
 * OLED VCC->3V3, GND->G, SCL->PB6, SDA->PB7
 * Shows uptime and KEY press count.
 * LED slow blink = OLED found, fast blink = check wires.
 */
#include "stm32f4xx.h"

#define OLED_ADDR  0x3CU

static volatile uint32_t ms;

void SysTick_Handler(void) { ms++; }

static void delay_ms(uint32_t d)
{
    uint32_t start = ms;
    while ((ms - start) < d) { }
}

static void delay_us(uint32_t us)
{
    volatile uint32_t n = us * 4U;
    while (n--) { __NOP(); }
}

/* ---------------- clock: fixed 16 MHz so I2C timing is right ---------------- */
static void clock_init(void)
{
    uint32_t t = 100000U;
    RCC->CR |= RCC_CR_HSION;
    while (!(RCC->CR & RCC_CR_HSIRDY) && --t) { }
    RCC->CFGR &= ~(RCC_CFGR_SW | RCC_CFGR_HPRE | RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2);
    t = 100000U;
    while (((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_HSI) && --t) { }
}

/* ---------------- I2C1 on PB6 (SCL) and PB7 (SDA), 100 kHz ---------------- */
static void i2c_init(void)
{
    uint8_t i;

    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    RCC->APB1ENR |= RCC_APB1ENR_I2C1EN;
    __DSB();

    /* free the bus in case the OLED was left mid-transfer */
    GPIOB->BSRR   = (1U << 6) | (1U << 7);
    GPIOB->OTYPER |= (1U << 6) | (1U << 7);
    GPIOB->PUPDR  = (GPIOB->PUPDR & ~((3U << 12) | (3U << 14))) | (1U << 12) | (1U << 14);
    GPIOB->MODER  = (GPIOB->MODER & ~((3U << 12) | (3U << 14))) | (1U << 12) | (1U << 14);
    for (i = 0; i < 9U && !(GPIOB->IDR & (1U << 7)); i++) {
        GPIOB->BSRR = (1U << (6 + 16)); delay_us(6);
        GPIOB->BSRR = (1U << 6);        delay_us(6);
    }

    /* PB6, PB7 -> I2C1 (AF4), open drain */
    GPIOB->AFR[0] = (GPIOB->AFR[0] & ~((0xFU << 24) | (0xFU << 28))) | (4U << 24) | (4U << 28);
    GPIOB->MODER  = (GPIOB->MODER & ~((3U << 12) | (3U << 14))) | (2U << 12) | (2U << 14);

    RCC->APB1RSTR |=  RCC_APB1RSTR_I2C1RST;
    RCC->APB1RSTR &= ~RCC_APB1RSTR_I2C1RST;
    I2C1->CR2   = 16U;      /* 16 MHz bus */
    I2C1->CCR   = 80U;      /* 100 kHz    */
    I2C1->TRISE = 17U;
    I2C1->CR1   = I2C_CR1_PE;
}

/* wait for a status bit; 0 = ok, 1 = timeout or no answer */
static uint8_t i2c_wait(uint32_t bit)
{
    uint32_t t = 20000U;
    while (!(I2C1->SR1 & bit)) {
        if (I2C1->SR1 & I2C_SR1_AF) {           /* device said NACK */
            I2C1->SR1 &= ~I2C_SR1_AF;
            I2C1->CR1 |= I2C_CR1_STOP;
            return 1;
        }
        if (!--t) { I2C1->CR1 |= I2C_CR1_STOP; i2c_init(); return 1; }
    }
    return 0;
}

/* send: control byte, then n data bytes. 0 = ok */
static uint8_t i2c_send(uint8_t control, const uint8_t *data, uint16_t n)
{
    uint32_t t = 20000U;
    while ((I2C1->SR2 & I2C_SR2_BUSY) && --t) { }

    I2C1->CR1 |= I2C_CR1_START;
    if (i2c_wait(I2C_SR1_SB)) return 1;
    I2C1->DR = (uint8_t)(OLED_ADDR << 1);
    if (i2c_wait(I2C_SR1_ADDR)) return 1;
    (void)I2C1->SR2;                            /* clears ADDR */

    if (i2c_wait(I2C_SR1_TXE)) return 1;
    I2C1->DR = control;
    while (n--) {
        if (i2c_wait(I2C_SR1_TXE)) return 1;
        I2C1->DR = *data++;
    }
    if (i2c_wait(I2C_SR1_BTF)) return 1;
    I2C1->CR1 |= I2C_CR1_STOP;
    return 0;
}

/* ---------------- SSD1306 ---------------- */
static const uint8_t font5x7[64][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},{0x00,0x1C,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1C,0x00},{0x14,0x08,0x3E,0x08,0x14},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02},{0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},
    {0x00,0x56,0x36,0x00,0x00},{0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},{0x32,0x49,0x79,0x41,0x3E},
    {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7F,0x00},{0x04,0x02,0x01,0x02,0x04},
    {0x40,0x40,0x40,0x40,0x40}
};

static uint8_t oled_init(void)
{
    static const uint8_t seq[] = {
        0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
        0x8D, 0x14, 0x20, 0x02, 0xA1, 0xC8, 0xDA, 0x12,
        0x81, 0x7F, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF
    };
    return i2c_send(0x00, seq, (uint16_t)sizeof(seq));
}

/* write one text row (0..7), max 21 characters, rest of row cleared */
static void oled_row(uint8_t row, const char *text)
{
    static uint8_t px[128];
    uint8_t cmd[3], col = 0, i, idx;
    char c;

    while (*text && col < 126U) {
        c = *text++;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);   /* uppercase font */
        if (c < 32 || c > 95) c = ' ';
        idx = (uint8_t)(c - 32);
        for (i = 0; i < 5U; i++) px[col++] = font5x7[idx][i];
        px[col++] = 0x00;
    }
    while (col < 128U) px[col++] = 0x00;

    cmd[0] = (uint8_t)(0xB0U | row);   /* page   */
    cmd[1] = 0x00;                     /* column */
    cmd[2] = 0x10;
    (void)i2c_send(0x00, cmd, 3);
    (void)i2c_send(0x40, px, 128);
}

/* number to text */
static void num_to_text(char *out, const char *label, uint32_t v)
{
    char t[10];
    uint8_t n = 0;
    while (*label) *out++ = *label++;
    do { t[n++] = (char)('0' + v % 10U); v /= 10U; } while (v);
    while (n) *out++ = t[--n];
    *out = '\0';
}

/* ---------------- main ---------------- */
int main(void)
{
    uint8_t ok, row, was_down = 0, down;
    uint32_t presses = 0, last_draw = 0, last_blink = 0, last_key = 0;
    char line[24];

    clock_init();
    SysTick_Config(16000U);                         /* 1 ms tick */

    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOCEN;
    GPIOC->MODER = (GPIOC->MODER & ~(3U << 26)) | (1U << 26);   /* PC13 LED  */
    GPIOA->MODER &= ~3U;                                         /* PA0 KEY   */
    GPIOA->PUPDR  = (GPIOA->PUPDR & ~3U) | 1U;

    i2c_init();
    delay_ms(100);                                  /* OLED power-up */
    ok = (uint8_t)(oled_init() == 0);

    if (ok) {
        for (row = 0; row < 8U; row++) oled_row(row, "");
        oled_row(0, "STM32 + OLED WORKING");
        oled_row(2, "GAS LEAK DETECTOR");
        oled_row(3, "23CSE304");
    }

    while (1) {
        /* count UKEY presses (with 50 ms debounce) */
        down = (uint8_t)((GPIOA->IDR & 1U) == 0U);
        if (down && !was_down && (ms - last_key) > 50U) { presses++; last_key = ms; }
        was_down = down;

        /* update numbers 4 times a second */
        if (ok && (ms - last_draw) >= 250U) {
            last_draw = ms;
            num_to_text(line, "UPTIME: ", ms / 1000U);
            oled_row(5, line);
            num_to_text(line, "KEY PRESSES: ", presses);
            oled_row(6, line);
        }

        /* LED: slow = OLED ok, fast = OLED missing */
        if ((ms - last_blink) >= (ok ? 500U : 100U)) {
            last_blink = ms;
            GPIOC->ODR ^= (1U << 13);
        }
    }
}
