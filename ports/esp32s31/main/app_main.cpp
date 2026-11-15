// ESP-IDF entry point.
//
// The shared firmware keeps a conventional int main() (src/main.cpp), which
// runs the USB/BT service loop and never returns. ESP-IDF's FreeRTOS startup
// calls app_main() on the main task instead, so bridge the two here rather than
// making the shared source care which platform it is on.
//
// The main task's stack comes from CONFIG_ESP_MAIN_TASK_STACK_SIZE.

extern int main();

extern "C" void app_main(void) {
    main();
}
