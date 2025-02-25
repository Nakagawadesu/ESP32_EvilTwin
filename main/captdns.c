/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * Jeroen Domburg <jeroen@spritesmods.com> wrote this file. As long as you retain
 * this notice you can do whatever you want with this stuff. If we meet some day,
 * and you think this stuff is worth it, you can buy me a beer in return.
 *
 * modified for ESP32 by Cornelis
 *
 * ----------------------------------------------------------------------------
 */

/*
This is a 'captive portal' DNS server: it basically replies with a fixed IP (in this case:
the one of the SoftAP interface of this ESP module) for any and all DNS queries. This can
be used to send mobile phones, tablets etc which connect to the ESP in AP mode directly to
the internal webserver.
*/

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "lwip/sockets.h"
#include "lwip/err.h"
#include "tcpip_adapter.h"
#include "string.h"

static int sockFd;

#define DNS_LEN 512

typedef struct __attribute__((packed))
{
	uint16_t id;
	uint8_t flags;
	uint8_t rcode;
	uint16_t qdcount;
	uint16_t ancount;
	uint16_t nscount;
	uint16_t arcount;
} DnsHeader;

typedef struct __attribute__((packed))
{
	uint8_t len;
	uint8_t data;
} DnsLabel;

typedef struct __attribute__((packed))
{
	// before: label
	uint16_t type;
	uint16_t class;
} DnsQuestionFooter;

typedef struct __attribute__((packed))
{
	// before: label
	uint16_t type;
	uint16_t class;
	uint32_t ttl;
	uint16_t rdlength;
	// after: rdata
} DnsResourceFooter;

typedef struct __attribute__((packed))
{
	uint16_t prio;
	uint16_t weight;
} DnsUriHdr;

#define FLAG_QR (1 << 7)
#define FLAG_AA (1 << 2)
#define FLAG_TC (1 << 1)
#define FLAG_RD (1 << 0)

#define QTYPE_A 1
#define QTYPE_NS 2
#define QTYPE_CNAME 5
#define QTYPE_SOA 6
#define QTYPE_WKS 11
#define QTYPE_PTR 12
#define QTYPE_HINFO 13
#define QTYPE_MINFO 14
#define QTYPE_MX 15
#define QTYPE_TXT 16
#define QTYPE_URI 256

#define QCLASS_IN 1
#define QCLASS_ANY 255
#define QCLASS_URI 256

// Function to put unaligned 16-bit network values
static void setn16(void *pp, int16_t n)
{
	char *p = pp;
	*p++ = (n >> 8);
	*p++ = (n & 0xff);
}

// Function to put unaligned 32-bit network values
static void setn32(void *pp, int32_t n)
{
	char *p = pp;
	*p++ = (n >> 24) & 0xff;
	*p++ = (n >> 16) & 0xff;
	*p++ = (n >> 8) & 0xff;
	*p++ = (n & 0xff);
}

static uint16_t my_ntohs(uint16_t *in)
{
	char *p = (char *)in;
	return ((p[0] << 8) & 0xff00) | (p[1] & 0xff);
}

// Parses a label into a C-string containing a dotted
// Returns pointer to start of next fields in packet
static char *labelToStr(char *packet, char *labelPtr, int packetSz, char *res, int resMaxLen)
{
	int i, j, k;
	char *endPtr = NULL;
	i = 0;
	do
	{
		if ((*labelPtr & 0xC0) == 0)
		{
			j = *labelPtr++; // skip past length
			// Add separator period if there already is data in res
			if (i < resMaxLen && i != 0)
				res[i++] = '.';
			// Copy label to res
			for (k = 0; k < j; k++)
			{
				if ((labelPtr - packet) > packetSz)
					return NULL;
				if (i < resMaxLen)
					res[i++] = *labelPtr++;
			}
		}
		else if ((*labelPtr & 0xC0) == 0xC0)
		{
			// Compressed label pointer
			endPtr = labelPtr + 2;
			int offset = my_ntohs(((uint16_t *)labelPtr)) & 0x3FFF;
			// Check if offset points to somewhere outside of the packet
			if (offset > packetSz)
				return NULL;
			labelPtr = &packet[offset];
		}
		// check for out-of-bound-ness
		if ((labelPtr - packet) > packetSz)
			return NULL;
	} while (*labelPtr != 0);
	res[i] = 0; // zero-terminate
	if (endPtr == NULL)
		endPtr = labelPtr + 1;
	return endPtr;
}

// Converts a dotted hostname to the weird label form dns uses.
static char *strToLabel(char *str, char *label, int maxLen)
{
	char *len = label;	 // ptr to len byte
	char *p = label + 1; // ptr to next label byte to be written
	while (1)
	{
		if (*str == '.' || *str == 0)
		{
			*len = ((p - len) - 1); // write len of label bit
			len = p;				// pos of len for next part
			p++;					// data ptr is one past len
			if (*str == 0)
				break; // done
			str++;
		}
		else
		{
			*p++ = *str++; // copy byte
			// if ((p-label)>maxLen) return NULL;	//check out of bounds
		}
	}
	*len = 0;
	return p; // ptr to first free byte in resp
}

// Receive a DNS packet and maybe send a response back
static void captdnsRecv(struct sockaddr_in *premote_addr, char *pusrdata, unsigned short length)
{

	char buff[DNS_LEN];
	char reply[DNS_LEN];
	int i;
	char *rend = &reply[length];
	char *p = pusrdata;
	DnsHeader *hdr = (DnsHeader *)p;
	DnsHeader *rhdr = (DnsHeader *)&reply[0];
	p += sizeof(DnsHeader);
	//	printf("DNS packet: id 0x%X flags 0x%X rcode 0x%X qcnt %d ancnt %d nscount %d arcount %d len %d\n",
	//		my_ntohs(&hdr->id), hdr->flags, hdr->rcode, my_ntohs(&hdr->qdcount), my_ntohs(&hdr->ancount), my_ntohs(&hdr->nscount), my_ntohs(&hdr->arcount), length);
	// Some sanity checks:
	if (length > DNS_LEN)
		return; // Packet is longer than DNS implementation allows
	if (length < sizeof(DnsHeader))
		return; // Packet is too short
	if (hdr->ancount || hdr->nscount || hdr->arcount)
		return; // this is a reply, don't know what to do with it
	if (hdr->flags & FLAG_TC)
		return; // truncated, can't use this
	// Reply is basically the request plus the needed data
	memcpy(reply, pusrdata, length);
	rhdr->flags |= FLAG_QR;

	for (i = 0; i < my_ntohs(&hdr->qdcount); i++)
	{
		// Grab the labels in the q string
		p = labelToStr(pusrdata, p, length, buff, sizeof(buff));
		if (p == NULL)
			return;
		DnsQuestionFooter *qf = (DnsQuestionFooter *)p;
		p += sizeof(DnsQuestionFooter);

		printf("DNS: Q (type 0x%X class 0x%X) for %s\n", my_ntohs(&qf->type), my_ntohs(&qf->class), buff);

		if (my_ntohs(&qf->type) == QTYPE_A)
		{
			// They want to know the IPv4 address of something.
			// Build the response.

			rend = strToLabel(buff, rend, sizeof(reply) - (rend - reply)); // Add the label
			if (rend == NULL)
				return;
			DnsResourceFooter *rf = (DnsResourceFooter *)rend;
			rend += sizeof(DnsResourceFooter);
			setn16(&rf->type, QTYPE_A);
			setn16(&rf->class, QCLASS_IN);
			setn32(&rf->ttl, 0);
			setn16(&rf->rdlength, 4); // IPv4 addr is 4 bytes;
			// Grab the current IP of the softap interface

			tcpip_adapter_ip_info_t info;
			// tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_ETH, &info);
			tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_AP, &info);
			// struct ip_info info;
			// wifi_get_ip_info(SOFTAP_IF, &info);
			*rend++ = ip4_addr1(&info.ip);
			*rend++ = ip4_addr2(&info.ip);
			*rend++ = ip4_addr3(&info.ip);
			*rend++ = ip4_addr4(&info.ip);
			setn16(&rhdr->ancount, my_ntohs(&rhdr->ancount) + 1);
			// printf("IP Address:  %s\n", ip4addr_ntoa(&info.ip));
			// printf("Added A rec to resp. Resp len is %d\n", (rend-reply));
		}
		else if (my_ntohs(&qf->type) == QTYPE_NS)
		{
			// Give ns server. Basically can be whatever we want because it'll get resolved to our IP later anyway.
			rend = strToLabel(buff, rend, sizeof(reply) - (rend - reply)); // Add the label
			DnsResourceFooter *rf = (DnsResourceFooter *)rend;
			rend += sizeof(DnsResourceFooter);
			setn16(&rf->type, QTYPE_NS);
			setn16(&rf->class, QCLASS_IN);
			setn16(&rf->ttl, 0);
			setn16(&rf->rdlength, 4);
			*rend++ = 2;
			*rend++ = 'n';
			*rend++ = 's';
			*rend++ = 0;
			setn16(&rhdr->ancount, my_ntohs(&rhdr->ancount) + 1);
			// printf("Added NS rec to resp. Resp len is %d\n", (rend-reply));
		}
		else if (my_ntohs(&qf->type) == QTYPE_URI)
		{
			// Give uri to us
			rend = strToLabel(buff, rend, sizeof(reply) - (rend - reply)); // Add the label
			DnsResourceFooter *rf = (DnsResourceFooter *)rend;
			rend += sizeof(DnsResourceFooter);
			DnsUriHdr *uh = (DnsUriHdr *)rend;
			rend += sizeof(DnsUriHdr);
			setn16(&rf->type, QTYPE_URI);
			setn16(&rf->class, QCLASS_URI);
			setn16(&rf->ttl, 0);
			setn16(&rf->rdlength, 4 + 16);
			setn16(&uh->prio, 10);
			setn16(&uh->weight, 1);
			memcpy(rend, "http://esp.nonet", 16);
			rend += 16;
			setn16(&rhdr->ancount, my_ntohs(&rhdr->ancount) + 1);
			// printf("Added NS rec to resp. Resp len is %d\n", (rend-reply));
		}
	}
	// Send the response
	// printf("Send response\n");
	sendto(sockFd, (uint8_t *)reply, rend - reply, 0, (struct sockaddr *)premote_addr, sizeof(struct sockaddr_in));
}
static void captdnsTask(void *pvParameters)
{
	// Declaração de uma estrutura do tipo `sockaddr_in` chamada `server_addr`.
	// - `sockaddr_in` é uma estrutura usada para armazenar informações de endereço IPv4.
	// - Ela contém campos como família de endereços, endereço IP e porta.
	// - A variável `server_addr` será usada para configurar o endereço do servidor DNS.
	struct sockaddr_in server_addr;

	// Declaração de uma variável `ret` do tipo `uint32_t`.
	// - Essa variável será usada para armazenar o valor de retorno de funções como `bind()` e `recvfrom()`.
	uint32_t ret;

	// Declaração de uma estrutura do tipo `sockaddr_in` chamada `from`.
	// - Essa estrutura será usada para armazenar o endereço do cliente que enviou uma solicitação DNS.
	struct sockaddr_in from;

	// Declaração de uma variável `fromlen` do tipo `socklen_t`.
	// - Essa variável armazenará o tamanho da estrutura `from`.
	// - É usada em funções como `recvfrom()` para indicar o tamanho da estrutura de endereço.
	socklen_t fromlen;

	// Declaração de um buffer `udp_msg` de tamanho `DNS_LEN` (512 bytes).
	// - Esse buffer será usado para armazenar os dados recebidos do cliente (a solicitação DNS).
	char udp_msg[DNS_LEN];

	// Inicializa a estrutura `server_addr` com zeros.
	// - `memset()` preenche a memória com um valor específico.
	// - Aqui, está preenchendo a estrutura `server_addr` com zeros para garantir que esteja limpa antes de configurá-la.
	memset(&server_addr, 0, sizeof(server_addr));

	// Configura a família de endereços da estrutura `server_addr` como `AF_INET`.
	// - `AF_INET` indica que o socket usará endereços IPv4.
	// - Isso é necessário para que o socket saiba que tipo de endereço IP ele vai manipular.
	server_addr.sin_family = AF_INET;

	// Configura o endereço IP do servidor como `INADDR_ANY`.
	// - `INADDR_ANY` é uma constante que faz o socket escutar em todos os endereços IP disponíveis na máquina.
	// - Isso é útil quando o servidor tem múltiplos endereços IP (por exemplo, Wi-Fi e Ethernet).
	server_addr.sin_addr.s_addr = INADDR_ANY;

	// Configura a porta do servidor como 53.
	// - `htons(53)` converte o número da porta (53) para o formato de rede (big-endian).
	// - A porta 53 é a porta padrão para o protocolo DNS.
	server_addr.sin_port = htons(53);

	// Configura o tamanho da estrutura `server_addr`.
	// - `sin_len` é um campo opcional em algumas implementações de socket.
	// - Aqui, está sendo definido como o tamanho da estrutura `server_addr`.
	server_addr.sin_len = sizeof(server_addr);

	// Loop para criar o socket UDP.
	// - O socket é criado usando a função `socket()`.
	// - Se a criação falhar (`sockFd == -1`), o loop tenta novamente após um delay de 1 segundo.
	do
	{
		// Cria um socket UDP.
		// - `AF_INET`: Família de endereços IPv4.
		// - `SOCK_DGRAM`: Tipo de socket (UDP).
		// - `0`: Protocolo padrão (UDP).
		// - Retorna um descritor de arquivo (`sockFd`) que representa o socket.
		sockFd = socket(AF_INET, SOCK_DGRAM, 0);

		// Verifica se a criação do socket falhou.
		if (sockFd == -1)
		{
			// Exibe uma mensagem de erro.
			printf("captdns_task failed to create sock!\n");

			// Aguarda 1 segundo antes de tentar novamente.
			vTaskDelay(1000 / portTICK_RATE_MS);
		}
	} while (sockFd == -1); // Repete o loop até que o socket seja criado com sucesso.

	// Loop para vincular o socket ao endereço do servidor.
	// - A função `bind()` associa o socket ao endereço IP e porta especificados.
	// - Se a vinculação falhar (`ret != 0`), o loop tenta novamente após um delay de 1 segundo.
	do
	{
		// Vincula o socket ao endereço do servidor.
		// - `sockFd`: Descritor do socket.
		// - `(struct sockaddr *)&server_addr`: Endereço do servidor.
		// - `sizeof(server_addr)`: Tamanho da estrutura.
		// - Retorna 0 em caso de sucesso.
		ret = bind(sockFd, (struct sockaddr *)&server_addr, sizeof(server_addr));

		// Verifica se a vinculação falhou.
		if (ret != 0)
		{
			// Exibe uma mensagem de erro.
			printf("captdns_task failed to bind sock!\n");

			// Aguarda 1 segundo antes de tentar novamente.
			vTaskDelay(1000 / portTICK_RATE_MS);
		}
	} while (ret != 0); // Repete o loop até que o socket seja vinculado com sucesso.

	// Exibe uma mensagem indicando que o servidor DNS foi inicializado.
	printf("CaptDNS inited.\n");

	// Loop principal do servidor DNS.
	// - Fica em um loop infinito recebendo pacotes UDP e processando solicitações DNS.
	while (1)
	{
		// Limpa a estrutura `from` com zeros.
		// - Isso garante que a estrutura esteja limpa antes de receber dados.
		memset(&from, 0, sizeof(from));

		// Define o tamanho da estrutura `from`.
		// - `fromlen` será usado pela função `recvfrom()` para saber o tamanho da estrutura.
		fromlen = sizeof(struct sockaddr_in);

		// Recebe um pacote UDP.
		// - `sockFd`: Descritor do socket.
		// - `(uint8_t *)udp_msg`: Buffer para armazenar os dados recebidos.
		// - `DNS_LEN`: Tamanho do buffer.
		// - `0`: Flags (nenhuma flag especial).
		// - `(struct sockaddr *)&from`: Endereço do cliente.
		// - `(socklen_t *)&fromlen`: Tamanho da estrutura `from`.
		// - Retorna o número de bytes recebidos.
		ret = recvfrom(sockFd, (uint8_t *)udp_msg, DNS_LEN, 0, (struct sockaddr *)&from, (socklen_t *)&fromlen);

		// Verifica se dados foram recebidos.
		if (ret > 0)
		{
			// Processa a solicitação DNS e envia uma resposta.
			// - `&from`: Endereço do cliente.
			// - `udp_msg`: Dados recebidos (solicitação DNS).
			// - `ret`: Número de bytes recebidos.
			captdnsRecv(&from, udp_msg, ret);
		}
	}

	// Fecha o socket.
	// - Libera os recursos do socket.
	close(sockFd);

	// Deleta a tarefa atual.
	// - Encerra a execução da tarefa.
	vTaskDelete(NULL);
}
void captdnsInit(void)
{
	xTaskCreate(captdnsTask, (const char *)"captdns_task", 10000, NULL, 3, NULL);
}
