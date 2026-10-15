/*
 * Blink + button test, Black Pill only, nothing else wired.
 * Press KEY (PA0) to change LED blink speed. Uses SysTick + EXTI0.
 * Copyright (c) 2026 Nimal Danyath K (nimaldanyathk). All rights reserved.
 */
#include "stm32f4xx.h"

static volatile uint32_t ms   = 0;   /* milliseconds since start */
static volatile uint8_t  mode = 0;   /* 0 slow, 1 medium, 2 fast */

/* Timer interrupt: runs every 1 ms */
void SysTick_Handler(void)
{
    ms++;
}

/* Button interrupt: runs when KEY (PA0) is pressed */
void EXTI0_IRQHandler(void)
{
    static uint32_t last = 0;
    EXTI->PR = EXTI_PR_PR0;              /* clear the interrupt flag */
    if (ms - last > 200U) {              /* ignore switch bounce     */
        last = ms;
        mode = (uint8_t)((mode + 1U) % 3U);
    }
}

int main(void)
{
    static const uint32_t half_period[3] = { 1000U, 250U, 60U };
    uint32_t t = 0;

    SysTick_Config(SystemCoreClock / 1000U);   /* 1 ms tick */

    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOCEN;
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;

    /* PC13 = LED, output */
    GPIOC->MODER = (GPIOC->MODER & ~(3U << 26)) | (1U << 26);

    /* PA0 = KEY, input with pull-up */
    GPIOA->MODER &= ~3U;
    GPIOA->PUPDR  = (GPIOA->PUPDR & ~3U) | 1U;

    /* Connect PA0 to EXTI line 0, trigger on press (falling edge) */
    SYSCFG->EXTICR[0] &= ~0xFU;
    EXTI->IMR  |= EXTI_IMR_MR0;
    EXTI->FTSR |= EXTI_FTSR_TR0;
    NVIC_EnableIRQ(EXTI0_IRQn);

    while (1) {
        if (ms - t >= half_period[mode]) {
            t = ms;
            GPIOC->ODR ^= (1U << 13);    /* toggle LED */
        }
    }
}
