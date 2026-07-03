idf_component_register(
  SRCS "idf_sms.cpp"
  INCLUDE_DIRS "include"
  REQUIRES
    esp_timer
    idf_config
    idf_inbox
    idf_logbuf
    idf_modem
    idf_pdu
    idf_push
)
