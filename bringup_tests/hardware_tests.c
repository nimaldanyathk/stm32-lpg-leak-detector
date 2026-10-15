/*
 * Hardware bring-up tests for the LPG detector
 * Copyright (c) 2026 Nimal Danyath K (nimaldanyathk). All rights reserved.
 *
 * Set TEST to 1..6, build, flash, go in order:
 *   1 blink  2 I2C scan  3 OLED  4 buzzer  5 MQ raw ADC  6 all + BT
 *
 * Same pins as the main firmware. MQ AO goes through the 10k/10k
 * divider, never straight to the pin (5 V vs 3.3 V).
 */

#define TEST   1

/* MQ-6 raw ADC count that triggers the buzzer in TEST 6. Tune later. */
#define MQ6_ALARM   1800

#include "stm32f4xx.h"

/* ---------------------------------------------------------------- */
/* timing                                                            */
/* ---------------------------------------------------------------- */
static volatile uint32_t ms_ticks;

void SysTick_Handler(void) { ms_ticks++; }

static void delay_ms(uint32_t ms)
{
    uint32_t start = ms_ticks;
    while ((ms_ticks - start) < ms) { __NOP(); }
}

static void delay_us_rough(uint32_t us)
{
    volatile uint32_t n = us * 3U;
    while (n--) { __NOP(); }
}

/* ---------------------------------------------------------------- */
/* USART1 on PA9 / PA10, 9600 baud, talks to the HC-05               */
/* ---------------------------------------------------------------- */
static void uart_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;

    GPIOA->MODER  &= ~((3U << (9 * 2)) | (3U << (10 * 2)));
    GPIOA->MODER  |=  ((2U << (9 * 2)) | (2U << (10 * 2)));
    GPIOA->AFR[1] &= ~((0xFU << ((9 - 8) * 4)) | (0xFU << ((10 - 8) * 4)));
    GPIOA->AFR[1] |=  ((7U   << ((9 - 8) * 4)) | (7U   << ((10 - 8) * 4)));

    USART1->BRR = 1667;                                    /* 9600 @ 16 MHz */
    USART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
}

static void uart_putc(char c)
{
    while (!(USART1->SR & USART_SR_TXE)) { }
    USART1->DR = (uint16_t)c;
}

static void uart_print(const char *s) { while (*s) uart_putc(*s++); }

static void uart_hex2(uint8_t v)
{
    const char *h = "0123456789ABCDEF";
    uart_putc(h[(v >> 4) & 0xF]);
    uart_putc(h[v & 0xF]);
}

static void uart_dec(uint32_t v)
{
    char b[11];
    int8_t i = 0;
    if (v == 0) { uart_putc('0'); return; }
    while (v) { b[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i--) uart_putc(b[i]);
}

/* ---------------------------------------------------------------- */
/* I2C1 on PB6 (SCL) / PB7 (SDA), 100 kHz                            */
/* ---------------------------------------------------------------- */
static void i2c_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    RCC->APB1ENR |= RCC_APB1ENR_I2C1EN;

    GPIOB->MODER   &= ~((3U << (6 * 2)) | (3U << (7 * 2)));
    GPIOB->MODER   |=  ((2U << (6 * 2)) | (2U << (7 * 2)));   /* AF */
    GPIOB->OTYPER  |=   (1U << 6) | (1U << 7);                /* open drain */
    GPIOB->OSPEEDR |=  ((3U << (6 * 2)) | (3U << (7 * 2)));
    GPIOB->PUPDR   &= ~((3U << (6 * 2)) | (3U << (7 * 2)));
    GPIOB->PUPDR   |=  ((1U << (6 * 2)) | (1U << (7 * 2)));   /* pull-up */
    GPIOB->AFR[0]  &= ~((0xFU << (6 * 4)) | (0xFU << (7 * 4)));
    GPIOB->AFR[0]  |=  ((4U   << (6 * 4)) | (4U   << (7 * 4)));  /* AF4 */

    I2C1->CR1 = I2C_CR1_SWRST;
    I2C1->CR1 = 0;
    I2C1->CR2   = 16;      /* APB1 = 16 MHz */
    I2C1->CCR   = 80;      /* 100 kHz */
    I2C1->TRISE = 17;
    I2C1->CR1  |= I2C_CR1_PE;
}

#define I2C_WAIT(cond)                                        \
    do {                                                      \
        uint32_t _t = 200000U;                                \
        while (!(cond)) {                                     \
            if (!--_t) { I2C1->CR1 |= I2C_CR1_STOP; return 1; } \
        }                                                     \
    } while (0)

/* returns 0 on success, 1 on failure */
static uint8_t i2c_write_buf(uint8_t addr7, const uint8_t *d, uint16_t n)
{
    I2C1->CR1 |= I2C_CR1_START;
    I2C_WAIT(I2C1->SR1 & I2C_SR1_SB);

    I2C1->DR = (uint8_t)(addr7 << 1);
    I2C_WAIT(I2C1->SR1 & (I2C_SR1_ADDR | I2C_SR1_AF));

    if (I2C1->SR1 & I2C_SR1_AF) {
        I2C1->SR1 &= ~I2C_SR1_AF;
        I2C1->CR1 |= I2C_CR1_STOP;
        return 1;
    }
    (void)I2C1->SR1; (void)I2C1->SR2;

    while (n--) {
        I2C_WAIT(I2C1->SR1 & I2C_SR1_TXE);
        I2C1->DR = *d++;
    }
    I2C_WAIT(I2C1->SR1 & I2C_SR1_BTF);
    I2C1->CR1 |= I2C_CR1_STOP;
    return 0;
}

static uint8_t i2c_probe(uint8_t addr7)
{
    uint32_t t;

    I2C1->CR1 |= I2C_CR1_START;
    t = 200000U;
    while (!(I2C1->SR1 & I2C_SR1_SB)) if (!--t) goto bad;

    I2C1->DR = (uint8_t)(addr7 << 1);
    t = 200000U;
    while (!(I2C1->SR1 & (I2C_SR1_ADDR | I2C_SR1_AF))) if (!--t) goto bad;

    if (I2C1->SR1 & I2C_SR1_ADDR) {
        (void)I2C1->SR1; (void)I2C1->SR2;
        I2C1->CR1 |= I2C_CR1_STOP;
        return 1;
    }
    I2C1->SR1 &= ~I2C_SR1_AF;
    I2C1->CR1 |= I2C_CR1_STOP;
    return 0;

bad:
    I2C1->CR1 |= I2C_CR1_STOP;
    i2c_init();
    return 0;
}

/* ---------------------------------------------------------------- */
/* SSD1306 128x64 OLED at 0x3C, page addressing, 5x7 font            */
/* ---------------------------------------------------------------- */
#define OLED_ADDR 0x3C

static const uint8_t font5x7[64][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /*   */
    {0x00,0x00,0x5F,0x00,0x00}, /* ! */
    {0x00,0x07,0x00,0x07,0x00}, /* " */
    {0x14,0x7F,0x14,0x7F,0x14}, /* # */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* $ */
    {0x23,0x13,0x08,0x64,0x62}, /* % */
    {0x36,0x49,0x55,0x22,0x50}, /* & */
    {0x00,0x05,0x03,0x00,0x00}, /* ' */
    {0x00,0x1C,0x22,0x41,0x00}, /* ( */
    {0x00,0x41,0x22,0x1C,0x00}, /* ) */
    {0x14,0x08,0x3E,0x08,0x14}, /* * */
    {0x08,0x08,0x3E,0x08,0x08}, /* + */
    {0x00,0x50,0x30,0x00,0x00}, /* , */
    {0x08,0x08,0x08,0x08,0x08}, /* - */
    {0x00,0x60,0x60,0x00,0x00}, /* . */
    {0x20,0x10,0x08,0x04,0x02}, /* / */
    {0x3E,0x51,0x49,0x45,0x3E}, /* 0 */
    {0x00,0x42,0x7F,0x40,0x00}, /* 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 2 */
    {0x21,0x41,0x45,0x4B,0x31}, /* 3 */
    {0x18,0x14,0x12,0x7F,0x10}, /* 4 */
    {0x27,0x45,0x45,0x45,0x39}, /* 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 6 */
    {0x01,0x71,0x09,0x05,0x03}, /* 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 8 */
    {0x06,0x49,0x49,0x29,0x1E}, /* 9 */
    {0x00,0x36,0x36,0x00,0x00}, /* : */
    {0x00,0x56,0x36,0x00,0x00}, /* ; */
    {0x08,0x14,0x22,0x41,0x00}, /* < */
    {0x14,0x14,0x14,0x14,0x14}, /* = */
    {0x00,0x41,0x22,0x14,0x08}, /* > */
    {0x02,0x01,0x51,0x09,0x06}, /* ? */
    {0x32,0x49,0x79,0x41,0x3E}, /* @ */
    {0x7E,0x11,0x11,0x11,0x7E}, /* A */
    {0x7F,0x49,0x49,0x49,0x36}, /* B */
    {0x3E,0x41,0x41,0x41,0x22}, /* C */
    {0x7F,0x41,0x41,0x22,0x1C}, /* D */
    {0x7F,0x49,0x49,0x49,0x41}, /* E */
    {0x7F,0x09,0x09,0x09,0x01}, /* F */
    {0x3E,0x41,0x49,0x49,0x7A}, /* G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* H */
    {0x00,0x41,0x7F,0x41,0x00}, /* I */
    {0x20,0x40,0x41,0x3F,0x01}, /* J */
    {0x7F,0x08,0x14,0x22,0x41}, /* K */
    {0x7F,0x40,0x40,0x40,0x40}, /* L */
    {0x7F,0x02,0x0C,0x02,0x7F}, /* M */
    {0x7F,0x04,0x08,0x10,0x7F}, /* N */
    {0x3E,0x41,0x41,0x41,0x3E}, /* O */
    {0x7F,0x09,0x09,0x09,0x06}, /* P */
    {0x3E,0x41,0x51,0x21,0x5E}, /* Q */
    {0x7F,0x09,0x19,0x29,0x46}, /* R */
    {0x46,0x49,0x49,0x49,0x31}, /* S */
    {0x01,0x01,0x7F,0x01,0x01}, /* T */
    {0x3F,0x40,0x40,0x40,0x3F}, /* U */
    {0x1F,0x20,0x40,0x20,0x1F}, /* V */
    {0x3F,0x40,0x38,0x40,0x3F}, /* W */
    {0x63,0x14,0x08,0x14,0x63}, /* X */
    {0x07,0x08,0x70,0x08,0x07}, /* Y */
    {0x61,0x51,0x49,0x45,0x43}, /* Z */
    {0x00,0x7F,0x41,0x41,0x00}, /* [ */
    {0x02,0x04,0x08,0x10,0x20}, /* \ */
    {0x00,0x41,0x41,0x7F,0x00}, /* ] */
    {0x04,0x02,0x01,0x02,0x04}, /* ^ */
    {0x40,0x40,0x40,0x40,0x40}  /* _ */
};

static uint8_t oled_buf[132];

static uint8_t oled_cmd(uint8_t c)
{
    uint8_t b[2];
    b[0] = 0x00;
    b[1] = c;
    return i2c_write_buf(OLED_ADDR, b, 2);
}

static uint8_t oled_data(const uint8_t *d, uint16_t n)
{
    uint16_t i;
    if (n > 130) n = 130;
    oled_buf[0] = 0x40;
    for (i = 0; i < n; i++) oled_buf[i + 1] = d[i];
    return i2c_write_buf(OLED_ADDR, oled_buf, (uint16_t)(n + 1));
}

static void oled_goto(uint8_t page, uint8_t col)
{
    oled_cmd((uint8_t)(0xB0 | (page & 0x07)));
    oled_cmd((uint8_t)(0x00 | (col & 0x0F)));
    oled_cmd((uint8_t)(0x10 | (col >> 4)));
}

static void oled_clear(void)
{
    uint8_t page, zeros[128];
    uint16_t i;
    for (i = 0; i < 128; i++) zeros[i] = 0x00;
    for (page = 0; page < 8; page++) {
        oled_goto(page, 0);
        oled_data(zeros, 128);
    }
}

static uint8_t oled_init(void)
{
    static const uint8_t seq[] = {
        0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
        0x8D, 0x14, 0x20, 0x02, 0xA1, 0xC8, 0xDA, 0x12,
        0x81, 0x7F, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6,
        0xAF
    };
    uint8_t i;
    delay_ms(100);
    for (i = 0; i < sizeof(seq); i++) {
        if (oled_cmd(seq[i])) return 1;      /* OLED did not answer */
    }
    oled_clear();
    return 0;
}

static void oled_putc(char c)
{
    uint8_t g[6];
    uint8_t i, idx;

    if (c >= 'a' && c <= 'z') c = (char)(c - 32);     /* no lowercase glyphs */
    if (c < 32 || c > 95) c = 32;
    idx = (uint8_t)(c - 32);

    for (i = 0; i < 5; i++) g[i] = font5x7[idx][i];
    g[5] = 0x00;
    oled_data(g, 6);
}

static void oled_print(uint8_t page, uint8_t col, const char *s)
{
    oled_goto(page, col);
    while (*s) oled_putc(*s++);
}

static void oled_print_dec(uint8_t page, uint8_t col, const char *label, uint32_t v)
{
    char b[12];
    int8_t i = 0, j;
    oled_goto(page, col);
    while (*label) oled_putc(*label++);
    if (v == 0) { oled_putc('0'); }
    else {
        while (v) { b[i++] = (char)('0' + (v % 10)); v /= 10; }
        for (j = (int8_t)(i - 1); j >= 0; j--) oled_putc(b[j]);
    }
    oled_putc(' '); oled_putc(' '); oled_putc(' ');   /* wipe old digits */
}

/* ---------------------------------------------------------------- */
/* buzzer on PB12                                                    */
/* ---------------------------------------------------------------- */
static void buzzer_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    GPIOB->MODER &= ~(3U << (12 * 2));
    GPIOB->MODER |=  (1U << (12 * 2));     /* output */
    GPIOB->ODR   &= ~(1U << 12);           /* off */
}

#define BUZZ_ON()   (GPIOB->ODR |=  (1U << 12))
#define BUZZ_OFF()  (GPIOB->ODR &= ~(1U << 12))

/* works on a passive buzzer: square wave at about 2 kHz */
static void buzzer_tone(uint32_t ms)
{
    uint32_t cycles = ms * 2U;
    while (cycles--) {
        BUZZ_ON();  delay_us_rough(250);
        BUZZ_OFF(); delay_us_rough(250);
    }
}

/* ---------------------------------------------------------------- */
/* ADC1: PA1 = IN1 (MQ6), PB0 = IN8 (MQ2), PB1 = IN9 (MQ135)         */
/* ---------------------------------------------------------------- */
#define CH_MQ6    1U
#define CH_MQ2    8U
#define CH_MQ135  9U

static void adc_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOBEN;
    RCC->APB2ENR |= RCC_APB2ENR_ADC1EN;

    GPIOA->MODER |= (3U << (1 * 2));                 /* PA1 analog */
    GPIOB->MODER |= (3U << (0 * 2)) | (3U << (1 * 2)); /* PB0, PB1 analog */
    GPIOA->PUPDR &= ~(3U << (1 * 2));
    GPIOB->PUPDR &= ~((3U << (0 * 2)) | (3U << (1 * 2)));

    ADC->CCR = 0;                                    /* ADC clock = PCLK2/2 */

    /* 480 cycle sample time on channels 1, 8, 9 (divider is 5k source) */
    ADC1->SMPR2 = (7U << (1 * 3)) | (7U << (8 * 3)) | (7U << (9 * 3));
    ADC1->SQR1  = 0;                                 /* one conversion */
    ADC1->CR1   = 0;                                 /* 12 bit */
    ADC1->CR2   = ADC_CR2_ADON;
    delay_ms(5);
}

static uint16_t adc_read(uint8_t ch)
{
    ADC1->SQR3 = ch;
    ADC1->CR2 |= ADC_CR2_SWSTART;
    while (!(ADC1->SR & ADC_SR_EOC)) { }
    return (uint16_t)ADC1->DR;
}

/* average of 8 reads, kills most of the noise */
static uint16_t adc_avg(uint8_t ch)
{
    uint32_t s = 0;
    uint8_t i;
    for (i = 0; i < 8; i++) s += adc_read(ch);
    return (uint16_t)(s >> 3);
}

/* ---------------------------------------------------------------- */
/* main                                                              */
/* ---------------------------------------------------------------- */
static void led_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN;
    GPIOC->MODER &= ~(3U << (13 * 2));
    GPIOC->MODER |=  (1U << (13 * 2));
    GPIOC->ODR   |=  (1U << 13);          /* off, active low */
}

#define LED_ON()   (GPIOC->ODR &= ~(1U << 13))
#define LED_OFF()  (GPIOC->ODR |=  (1U << 13))

int main(void)
{
    SysTick_Config(16000U);               /* 1 ms tick at 16 MHz HSI */
    led_init();

/* ---- test 1 ---- */
#if TEST == 1
    while (1) {
        LED_ON();  delay_ms(300);
        LED_OFF(); delay_ms(300);
    }

/* ---- test 2 ---- */
#elif TEST == 2
    {
        uint8_t a, count = 0;

        uart_init();
        i2c_init();
        delay_ms(200);
        uart_print("\r\nI2C SCAN\r\n");

        for (a = 0x08; a <= 0x77; a++) {
            if (i2c_probe(a)) {
                uart_print("found 0x"); uart_hex2(a); uart_print("\r\n");
                count++;
            }
        }
        uart_print("total "); uart_dec(count); uart_print("\r\n");

        while (1) {                       /* blink once per device found */
            uint8_t i;
            for (i = 0; i < count; i++) {
                LED_ON();  delay_ms(150);
                LED_OFF(); delay_ms(250);
            }
            delay_ms(1500);
        }
    }

/* ---- test 3 ---- */
#elif TEST == 3
    {
        uint32_t n = 0;

        uart_init();
        i2c_init();

        if (oled_init()) {
            uart_print("OLED NOT RESPONDING\r\n");
            while (1) { LED_ON(); delay_ms(80); LED_OFF(); delay_ms(80); }
        }

        oled_print(0, 0, "GAS LEAK DETECTOR");
        oled_print(2, 0, "OLED OK");
        oled_print(3, 0, "I2C1 PB6 PB7");

        while (1) {
            oled_print_dec(5, 0, "UPTIME ", n++);
            LED_ON();  delay_ms(500);
            LED_OFF(); delay_ms(500);
        }
    }

/* ---- test 4 ---- */
#elif TEST == 4
    {
        buzzer_init();
        uart_init();
        uart_print("\r\nBUZZER TEST\r\n");

        while (1) {
            /* phase A: steady high. An ACTIVE buzzer sounds here. */
            uart_print("A steady\r\n");
            LED_ON();
            BUZZ_ON();  delay_ms(600);
            BUZZ_OFF(); LED_OFF();
            delay_ms(1000);

            /* phase B: 2 kHz square. A PASSIVE buzzer sounds here. */
            uart_print("B tone\r\n");
            LED_ON();
            buzzer_tone(600);
            BUZZ_OFF(); LED_OFF();
            delay_ms(2000);
        }
    }

/* ---- test 5 ---- */
#elif TEST == 5
    {
        uart_init();
        adc_init();
        uart_print("\r\nMQ RAW ADC  mq6,mq2,mq135\r\n");

        while (1) {
            uint16_t mq6   = adc_avg(CH_MQ6);
            uint16_t mq2   = adc_avg(CH_MQ2);
            uint16_t mq135 = adc_avg(CH_MQ135);

            uart_dec(mq6);   uart_putc(',');
            uart_dec(mq2);   uart_putc(',');
            uart_dec(mq135); uart_print("\r\n");

            LED_ON();  delay_ms(50);
            LED_OFF(); delay_ms(450);
        }
    }

/* ---- test 6 ---- */
#elif TEST == 6
    {
        uint8_t have_oled;
        uint32_t t = 0;

        uart_init();
        i2c_init();
        adc_init();
        buzzer_init();

        have_oled = (oled_init() == 0);
        if (have_oled) oled_print(0, 0, "GAS DETECTOR");

        uart_print("\r\nt,mq6,mq2,mq135,alarm\r\n");

        while (1) {
            uint16_t mq6   = adc_avg(CH_MQ6);
            uint16_t mq2   = adc_avg(CH_MQ2);
            uint16_t mq135 = adc_avg(CH_MQ135);
            uint8_t  alarm = (mq6 > MQ6_ALARM) ? 1 : 0;

            if (have_oled) {
                oled_print_dec(2, 0, "MQ6   ", mq6);
                oled_print_dec(3, 0, "MQ2   ", mq2);
                oled_print_dec(4, 0, "MQ135 ", mq135);
                oled_print(6, 0, alarm ? "** GAS ALARM **" : "STATUS SAFE    ");
            }

            uart_dec(t);     uart_putc(',');
            uart_dec(mq6);   uart_putc(',');
            uart_dec(mq2);   uart_putc(',');
            uart_dec(mq135); uart_putc(',');
            uart_dec(alarm); uart_print("\r\n");

            if (alarm) { LED_ON();  buzzer_tone(120); BUZZ_OFF(); }
            else       { LED_OFF(); BUZZ_OFF(); }

            t++;
            delay_ms(100);                /* 10 Hz, matches the ML window */
        }
    }

#else
    #error "Set TEST to 1..6"
#endif
}
