idf_component_register(
    SRCS "ai_service.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server gemini_service json localai_service nvs_flash recording_service
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
