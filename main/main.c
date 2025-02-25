/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42): Idea of Jeroen Domburg <jeroen@spritesmods.com>
 *
 * Cornelis wrote this file. As long as you retain
 * this notice you can do whatever you want with this stuff. If we meet some day,
 * and you think this stuff is worth it, you can buy me a beer in return.
 * ----------------------------------------------------------------------------
 */

#include "freertos/FreeRTOS.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "freertos/portmacro.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "tcpip_adapter.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/api.h"
#include "lwip/err.h"
#include "string.h"
#include "cJSON.h"
#include "lwip/dns.h"
#include "captdns.h"

#define LED_GPIO_PIN GPIO_NUM_4

#define LED_BUILTIN 16
#define delay(ms) (vTaskDelay(ms / portTICK_RATE_MS))

static esp_err_t event_handler(void *ctx, system_event_t *event);
static void wifi_AP_init(void);
static EventGroupHandle_t wifi_event_group;

const int CONNECTED_BIT = BIT0;

char *json_unformatted;

/*
const static char http_index_hml[] = "<!DOCTYPE html>"
      "<html>\n"
      "<head>\n"
      "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
      "  <style type=\"text/css\">\n"
      "    html, body, iframe { margin: 0; padding: 0; height: 100%; }\n"
      "    iframe { display: block; width: 100%; border: none; }\n"
      "  </style>\n"
      "<title>HELLO ESP32</title>\n"
      "</head>\n"
      "<body>\n"
      "<h1>Hello World, from ESP32!</h1>\n"
      "</body>\n"
      "</html>\n";
*/

const static char http_html_hdr[] = "HTTP/1.1 200 OK\r\nContent-type: text/html\r\n\r\n";
const static char http_logo_hdr[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: image/svg+xml\r\n"
    "Content-Length: 4169\r\n"
    "Connection: keep-alive\r\n"
    "Cache-Control: public, max-age=3600\r\n"
    "Last-Modified: Tue, 07 Jun 2022 13:51:30 GMT\r\n"
    "ETag: \"1049-5e0dbe2c19581\"\r\n"
    "Accept-Ranges: bytes\r\n"
    "\r\n";

const uint8_t indexHtmlStart[] asm("_binary_index_html_start"); // uint8_t
const uint8_t indexHtmlEnd[] asm("_binary_index_html_end");     // uint8_t
//
const uint8_t logoStart[] asm("_binary_logo_svg_start"); // uint8_t
const uint8_t logoEnd[] asm("_binary_logo_svg_end");     // uint8_t

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
  // Imprime o ID do evento recebido.
  printf("Event = %04X\n", event->event_id);

  // Verifica o tipo de evento.
  switch (event->event_id)
  {
  // Evento: O modo station (STA) foi iniciado.
  case SYSTEM_EVENT_STA_START:
    // Tenta conectar ao ponto de acesso Wi-Fi.
    esp_wifi_connect();
    break;

  // Evento: O ESP32 obteve um endereço IP.
  case SYSTEM_EVENT_STA_GOT_IP:
    // Define o bit CONNECTED_BIT no grupo de eventos para indicar que está conectado.
    xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);

    // Imprime o endereço IP, máscara de rede e gateway.
    printf("got ip\n");
    printf("ip: " IPSTR "\n", IP2STR(&event->event_info.got_ip.ip_info.ip));
    printf("netmask: " IPSTR "\n", IP2STR(&event->event_info.got_ip.ip_info.netmask));
    printf("gw: " IPSTR "\n", IP2STR(&event->event_info.got_ip.ip_info.gw));
    printf("\n");
    fflush(stdout);
    break;

  // Evento: O ESP32 foi desconectado do ponto de acesso.
  case SYSTEM_EVENT_STA_DISCONNECTED:
    // Tenta reconectar ao ponto de acesso.
    esp_wifi_connect();

    // Limpa o bit CONNECTED_BIT no grupo de eventos para indicar que está desconectado.
    xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
    break;

  // Outros eventos não são tratados.
  default:
    break;
  }

  // Retorna ESP_OK para indicar que o evento foi processado com sucesso.
  return ESP_OK;
}

static void http_server_netconn_serve(struct netconn *conn)
{
  // Declara variáveis para o buffer de recebimento de dados.
  struct netbuf *inbuf;
  char *buf;
  u16_t buflen;
  err_t err;

  // Recebe dados da conexão (bloqueia até que dados estejam disponíveis).
  err = netconn_recv(conn, &inbuf);

  // Se os dados forem recebidos com sucesso (ERR_OK), processa a requisição.
  if (err == ERR_OK)
  {
    // Obtém os dados recebidos e seu tamanho.
    netbuf_data(inbuf, (void **)&buf, &buflen);

    // Verifica se a requisição tem pelo menos 5 bytes (tamanho mínimo para "GET /").
    if (buflen >= 5)
    {
      // Verifica se a requisição é um GET.
      if (buf[0] == 'G' && buf[1] == 'E' && buf[2] == 'T' && buf[3] == ' ' && buf[4] == '/')
      {
        // Verifica se a requisição é para o caminho "/logo" (ou similar).
        if (buf[5] == 'l')
        {
          // Envia o cabeçalho HTTP para uma resposta SVG.
          netconn_write(conn, http_logo_hdr, sizeof(http_logo_hdr) - 1, NETCONN_NOCOPY);

          // Envia o conteúdo do arquivo SVG (armazenado na memória flash).
          netconn_write(conn, logoStart, 4169, NETCONN_NOCOPY);
        }
        else
        {
          // Envia o cabeçalho HTTP para uma resposta HTML.
          netconn_write(conn, http_html_hdr, sizeof(http_html_hdr) - 1, NETCONN_NOCOPY);

          // Envia o conteúdo do arquivo HTML (armazenado na memória flash).
          netconn_write(conn, indexHtmlStart, indexHtmlEnd - indexHtmlStart - 1, NETCONN_NOCOPY);
        }
      }
      // Verifica se a requisição é um POST.
      else if (buf[0] == 'P' && buf[1] == 'O' && buf[2] == 'S' && buf[3] == 'T' && buf[4] == ' ' && buf[5] == '/')
      {
        // Processa a requisição POST (neste caso, apenas imprime os dados recebidos).
        char *body = strstr(buf, "\r\n\r\n"); // Encontra o início do corpo da requisição.
        if (body != NULL)
        {
          body += 4;                                      // Pula os "\r\n\r\n" para chegar ao corpo.
          printf("\n###### Dados recebidos: %s\n", body); // Exibe os dados recebidos.

          // Responde com uma mensagem JSON de sucesso.
          const char *response = "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: application/json\r\n"
                                 "Connection: close\r\n"
                                 "\r\n"
                                 "{\"success\": true, \"message\": \"Login bem-sucedido!\"}";
          netconn_write(conn, response, strlen(response), NETCONN_NOCOPY);
        }
      }
    }
  }

  // Fecha a conexão após processar a requisição.
  netconn_close(conn);

  // Deleta o buffer de recebimento de dados.
  netbuf_delete(inbuf);
}
static void http_server(void *pvParameters)
{
  // Declara variáveis para o socket TCP e novas conexões.
  struct netconn *conn, *newconn;
  err_t err;

  // Cria um novo socket TCP.
  conn = netconn_new(NETCONN_TCP);

  // Vincula o socket à porta 80 (porta padrão para HTTP).
  netconn_bind(conn, NULL, 80);

  // Coloca o socket em modo de escuta, aguardando conexões de clientes.
  netconn_listen(conn);

  // Loop principal do servidor HTTP.
  do
  {
    // Aguarda uma nova conexão de cliente.
    err = netconn_accept(conn, &newconn);

    // Se uma conexão for aceita com sucesso (ERR_OK), processa a requisição.
    if (err == ERR_OK)
    {
      // Chama a função para processar a requisição HTTP.
      http_server_netconn_serve(newconn);

      // Deleta a conexão após processar a requisição.
      netconn_delete(newconn);
    }
  } while (err == ERR_OK); // Continua no loop enquanto não houver erros.

  // Fecha o socket principal.
  netconn_close(conn);

  // Deleta o socket principal.
  netconn_delete(conn);
}

int app_main(void)
{
  wifi_AP_init();

  // ip_addr_t dns_addr;
  // IP_ADDR4(&dns_addr, 192,168,4,100);
  // dns_setserver(0, &dns_addr);
  // dns_init();

  captdnsInit();
  xTaskCreate(&http_server, "http_server", 2048, NULL, 5, NULL);
  return 0;
}

void wifi_AP_init(void)
{
  // Inicializa o NVS (Non-Volatile Storage) para armazenar configurações.
  nvs_flash_init();

  // Inicializa o adaptador TCP/IP.
  tcpip_adapter_init();

  // Configura o event handler para lidar com eventos de rede.
  // O event_handler será chamado automaticamente quando ocorrerem eventos como conexão Wi-Fi, obtenção de IP, etc.
  ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));

  // Configura a estrutura de inicialização do Wi-Fi com valores padrão.
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

  // Inicializa o Wi-Fi com a configuração definida.
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  // Define que as configurações do Wi-Fi serão armazenadas na RAM (não persistirão após reinicialização).
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

  // Configura o Wi-Fi para operar no modo Access Point (AP).
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

  // Define as configurações do Access Point (AP).
  wifi_config_t apConfig = {
      .ap = {
          .ssid = "ESP32",            // Nome da rede Wi-Fi (SSID).
          .password = "test",         // Senha da rede Wi-Fi.
          .channel = 6,               // Canal de operação do Wi-Fi.
          .authmode = WIFI_AUTH_OPEN, // Modo de autenticação (aberto, sem senha).
          .max_connection = 4,        // Número máximo de dispositivos conectados.
          .beacon_interval = 100      // Intervalo de beacon em milissegundos.
      }};

  // Aplica as configurações do AP ao Wi-Fi.
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &apConfig));

  // Inicia o Wi-Fi com as configurações definidas.
  ESP_ERROR_CHECK(esp_wifi_start());
}
