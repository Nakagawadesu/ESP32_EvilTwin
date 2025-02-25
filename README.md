# Evil Twin Attack na UTFPR :rotating_light:
 
Este projeto é um trabalho da disciplina de Sistemas Embarcados e consiste na implementação de um ataque Evil Twin utilizando um ESP32. O objetivo é demonstrar na prática como redes Wi-Fi falsas podem ser usadas para capturar credenciais de usuários desavisados.


O código configura o ESP32 para atuar como um ponto de acesso falso (Evil Twin), replicando a identidade visual de uma rede legítima. Ao se conectar, o usuário é redirecionado para uma página de login falsa semelhante à utilizada na UTFPR, onde suas credenciais podem ser registradas para análise acadêmica e educacional sobre segurança de redes.

## Funcionalidades

- Criação de um AP falso: O ESP32 emula uma rede Wi-Fi com o mesmo SSID de uma rede legítima.

- Captive Portal: Qualquer tentativa de navegação redireciona o usuário para uma página de login falsa.

- Página HTML embutida: A página de login foi desenvolvida com um design similar ao da UTFPR e está incorporada no firmware para funcionar offline.

- Captura de credenciais: O sistema registra os dados inseridos para fins didáticos (sem uso malicioso).

## Como Utilizar

- Clone o repositório e instale o ESP-IDF:
  
```
git clone https://github.com/seuusuario/evil-twin-utfpr.git
cd evil-twin-utfpr
. $HOME/esp/esp-idf/export.sh
```

- Compile e grave no ESP32:
```
idf.py build flash monitor
```
> Lembre se de Rodar o script serialListener.py para capturar os dados que vem do cabo serial

- Conecte-se à rede Wi-Fi falsa e teste o funcionamento do captive portal.

## Créditos :alien:

O código original (captdns.c) foi escrito por cornelis-61 (se encontrá-lo pague uma cerveja), baseado na versão esp-idf-v2.0-rc1 da Espressif (2017). Posteriormente, smnd96 corrigiu a estrutura do projeto em 2020 para compatibilidade com esp-idf-v4.0.

> Aviso: Este projeto é estritamente educacional e não deve ser utilizado para fins ilegais. O uso indevido pode violar leis de privacidade e segurança cibernética.

## O que falta para ser um ataque completo :bulb:
- Deautenticação de pessoas próximas
- Uma página web mais convicente
- Um raio de abrangencia maior
- Modo Station que fornece internet depois de se autenticar no Soft Access Point
- Captura de dado sem se conectar ao computador
- Atualizar para a versão atual do esp-idf
