#include "nc.h"

void r2d2_play_happy(void) {
    hw_buzzer_play_tone(2000, 100);
    hw_buzzer_play_tone(2500, 100);
    hw_buzzer_play_tone(3000, 100);
    hw_buzzer_play_tone(2000, 100);
    hw_buzzer_play_tone(3500, 200);
}

void r2d2_play_sad(void) {
    hw_buzzer_play_tone(1000, 200);
    hw_buzzer_play_tone(800, 200);
    hw_buzzer_play_tone(600, 400);
}

void r2d2_play_confused(void) {
    hw_buzzer_play_tone(2000, 150);
    hw_buzzer_play_tone(1000, 150);
    hw_buzzer_play_tone(1500, 150);
}

void r2d2_play_excited(void) {
    hw_buzzer_play_tone(2000, 50);
    hw_buzzer_play_tone(2200, 50);
    hw_buzzer_play_tone(2500, 50);
    hw_buzzer_play_tone(2800, 50);
    hw_buzzer_play_tone(3000, 50);
    hw_buzzer_play_tone(3500, 200);
}

void r2d2_play_agree(void) {
    hw_buzzer_play_tone(2000, 80);
    hw_buzzer_play_tone(2500, 150);
}

void r2d2_play_disagree(void) {
    hw_buzzer_play_tone(1500, 150);
    hw_buzzer_play_tone(1200, 200);
}

void r2d2_play_processing(void) {
    hw_buzzer_play_tone(1500, 40);
    hw_buzzer_play_tone(1800, 40);
    hw_buzzer_play_tone(1200, 40);
    hw_buzzer_play_tone(1900, 40);
    hw_buzzer_play_tone(1400, 40);
    hw_buzzer_play_tone(2100, 40);
}

void r2d2_play_alert(void) {
    hw_buzzer_play_tone(3000, 100);
    hw_buzzer_play_tone(4000, 100);
    hw_buzzer_play_tone(3000, 100);
    hw_buzzer_play_tone(4000, 100);
    hw_buzzer_play_tone(3000, 100);
    hw_buzzer_play_tone(4000, 100);
}
