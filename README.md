# Visão · Firmware da ESP32-CAM

Firmware em C++ para a câmera da demonstração de Fundamentos de IoT da UFMS. O sensor gera JPEG e a placa transmite os quadros. A detecção e o reconhecimento facial rodam no computador.

## Placa usada

| Item | Especificação |
|---|---|
| Placa e alvo | ESP32-CAM de pinagem **AI-Thinker** · `esp32:esp32:esp32cam` |
| Módulo e chip | **ESP32-S**, chip identificado como **ESP32-D0WD-V3** (família ESP32 clássica) |
| Sensor | **OV3660**, saída JPEG |
| PSRAM | Cerca de **4 MiB** detectados na placa da bancada. O firmware consulta a PSRAM em execução e tem recuperação em QVGA. |
| Rede e alimentação | Wi-Fi de **2,4 GHz**, alimentação USB de **5 V** pelo adaptador da ESP32-CAM |
| Gravação | Adaptador USB–serial com botões **IO0/BOOT** e **RST**, porta serial variável por PC |
| Serial | **115200 baud** |

Pinagem da câmera no sketch: PWDN 32, XCLK 0, SCCB SDA 26 e SCL 27, D0–D7 em 5/18/19/21/36/39/34/35, VSYNC 25, HREF 23, PCLK 22, LED 4. A recuperação pode baixar a resolução e o clock quando a captura falha. A resolução selecionada é um teto. **VGA (640 × 480)** é um ponto de partida para a demonstração.

## Gravar na placa

1. Na Arduino IDE, instale **esp32 by Espressif Systems 3.3.12**. Abra `firmware/esp32_push/esp32_push.ino` e selecione **AI Thinker ESP32-CAM**. Selecione a porta COM do adaptador.
2. Para UDP local, compile direto. Para o vídeo do painel remoto, obtenha o **host, a porta UDP, o ID da câmera e a chave de publicação** com quem opera o receptor. Copie `esp32_receiver_config.example.h` para `esp32_receiver_config.h` na pasta do sketch e preencha os dados nessa cópia privada. Para HTTPS com uma CA diferente da incluída, siga os comentários do cabeçalho.
3. Feche o Monitor Serial. Segure **IO0**, aperte e solte **RST** e faça o upload. Se o adaptador não entrar em modo de gravação, mantenha **IO0** pressionado até o gravador conectar. Depois que a gravação for verificada, solte **IO0** e aperte **só RST**, uma vez.
4. Para configurar o Wi-Fi, aperte **RST três vezes rapidamente** com IO0 solto. Isso limpa as preferências salvas. Conecte-se à rede aberta **ESP32-CAM-…**, abra `http://192.168.4.1` e preencha o Wi-Fi, a resolução e o modo. O flash pisca rápido no portal, devagar ao conectar e duas vezes rápido quando o Wi-Fi conecta. Depois fica apagado.
5. Confira a linha de destino no Monitor Serial e veja se o receptor mostra vídeo. As duas piscadas rápidas confirmam o Wi-Fi. A chegada de quadros é verificada no receptor.

Arduino CLI, executando na raiz do repositório. Use o gerenciador da Arduino IDE para instalar o pacote ou o [índice de placas da Espressif](https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html).

```sh
arduino-cli compile --fqbn esp32:esp32:esp32cam firmware/esp32_push
arduino-cli upload -p COM6 --fqbn esp32:esp32:esp32cam firmware/esp32_push
arduino-cli monitor -p COM6 --config baudrate=115200
```

Substitua `COM6` pela porta que aparecer no seu PC. Se faltar sinal depois de gravar, confira se IO0 está solto, o Wi-Fi foi configurado e o modo selecionado corresponde ao receptor.

## Modos de vídeo

- **Somente local (UDP)** usa `udp://IP_DA_PLACA:5001`. O cliente precisa implementar a assinatura `ESP32CAM_SUB1` e remontar os JPEGs ECJ1. Na bancada, o Visão recebeu vídeo da placa física nesse modo.
- **UDP para o gateway** envia JPEGs ECJ2 protegidos por AES-256-GCM. A placa inicia o envio para o receptor configurado. Esse modo foi testado com a placa física.
- **HTTPS para o gateway** publica JPEGs por POST com autenticação e validação TLS. O endpoint foi testado com JPEGs de teste. O modo está disponível no portal e requer um receptor compatível e uma CA válida.

O painel web, o receptor e as credenciais de publicação são configurados à parte. No modo remoto o endereço de reprodução vem do receptor. Para gravar a placa da apresentação e manter o vídeo no painel remoto, use as credenciais privadas dela e configure o modo remoto no portal.
