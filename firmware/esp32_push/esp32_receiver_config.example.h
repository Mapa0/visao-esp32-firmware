#pragma once

// Para publicar pela internet, copie este arquivo para esp32_receiver_config.h.
// Preencha os dados recebidos do operador do gateway na sua cópia local.
#define ESP32_RECEIVER_HOST "gateway.example.invalid"
#define ESP32_RECEIVER_PORT 5002
#define ESP32_CAMERA_ID "PREENCHA_32_CARACTERES_HEXADECIMAIS"
#define ESP32_PUBLISH_KEY "PREENCHA_64_CARACTERES_HEXADECIMAIS"

// 0: UDP como padrão, usando as CAs incluídas se HTTPS for escolhido no portal.
// 1: HTTPS como padrão, usando ESP32_ROOT_CA abaixo.
#define ESP32_USE_HTTPS 0
static const char ESP32_ROOT_CA[] = R"PEM(
COLE_O_CERTIFICADO_PUBLICO_DA_CA_DO_RECEPTOR_AQUI
)PEM";
