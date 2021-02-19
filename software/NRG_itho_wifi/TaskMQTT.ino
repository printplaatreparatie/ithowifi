#if defined (__HW_VERSION_TWO__)



void TaskMQTT( void * pvParameters ) {
  configASSERT( ( uint32_t ) pvParameters == 1UL );


  mqttInit();

  startTaskWeb();

  esp_task_wdt_init(5, true);
  esp_task_wdt_add(NULL);

  for (;;) {
    yield();
    esp_task_wdt_reset();

    TaskMQTTTimeout.once_ms(1000, []() {
      logInput("Error: Task MQTT timed out!");
    });

    execMQTTTasks();

    TaskMQTTHWmark = uxTaskGetStackHighWaterMark( NULL );
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }

  //else delete task
  vTaskDelete( NULL );
}

#endif
