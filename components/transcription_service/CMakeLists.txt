idf_component_register(
    SRCS "transcription_service.cpp"
    INCLUDE_DIRS "include"
    REQUIRES ai_service esp_http_server recording_service task_config
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
